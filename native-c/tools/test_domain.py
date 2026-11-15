#!/usr/bin/env python3
"""Local-only domain tests: PTY modem, exact Go/C oracle, optional sanitizers."""
import argparse
import json
import os
from pathlib import Path
import pty
import random
import select
import shlex
import shutil
import subprocess
import tempfile
import threading
import tty

ROOT = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument("--deps", type=Path, default=Path(os.environ.get(
    "QDC_DEPS", str(Path.home() / ".cache/qdc507-native-c/host/prefix"))))
ap.add_argument("--build", type=Path, default=ROOT / "build/domain-tests")
ap.add_argument("--sanitize", action="store_true")
args = ap.parse_args()
args.build.mkdir(parents=True, exist_ok=True)
flags = shlex.split(os.environ.get("CC", "cc")) + ["-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Wpedantic",
         "-g", "-O1", "-I" + str(ROOT / "include"), "-I" + str(args.deps / "include")]
if args.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]


def build(name, sources, libs=()):
    out = args.build / name
    command = flags + [str(ROOT / s) for s in sources] + [
        "-L" + str(args.deps / "lib"), "-lyyjson", "-pthread",
    ] + list(libs) + ["-o", str(out)]
    subprocess.run(command, check=True)
    return out


pdu = build("test_pdu", ["tests/test_domain_pdu.c", "src/pdu.c", "src/common.c"])
subprocess.run([pdu], check=True)
oracle = build("pdu_oracle", ["tests/domain_oracle.c", "src/pdu.c", "src/common.c"])

# Build the original Go implementation in a disposable directory, offline.
with tempfile.TemporaryDirectory(prefix="qdc-domain-oracle-") as tmp:
    tmp = Path(tmp)
    (tmp / "pdu.go").write_bytes((ROOT.parent / "pdu.go").read_bytes())
    (tmp / "oracle.go").write_text('''package main
import("os";"bufio";"encoding/json")
func main(){s:=bufio.NewScanner(os.Stdin);s.Buffer(make([]byte,4096),1<<20);w:=json.NewEncoder(os.Stdout)
for s.Scan(){var r struct{Number string;Text string;Ref int};if json.Unmarshal(s.Bytes(),&r)!=nil{os.Exit(2)}
p,e:=buildSubmit(r.Number,r.Text,r.Ref);v:=map[string]interface{}{"ok":e==nil};if e==nil{v["pdus"]=p};w.Encode(v)}}''')
    env = os.environ.copy()
    goroot = os.environ.get("QDC_GO_ROOT")
    go = os.environ.get("QDC_GO") or shutil.which("go")
    if not go and goroot:
        go = str(Path(goroot) / "bin/go")
    if not go:
        raise RuntimeError("Go unavailable: install Go or set QDC_GO / QDC_GO_ROOT")
    env.pop("GOROOT", None)
    for key in ["GOFLAGS", "GOOS", "GOARCH"]:
        env.pop(key, None)
    if goroot:
        env["GOROOT"] = goroot
    env.update(GOENV="off", GOTOOLCHAIN="local", GOPROXY="off", GOSUMDB="off")
    gobin = args.build / "go_oracle"
    subprocess.run([go, "build", "-o", str(gobin), str(tmp / "pdu.go"), str(tmp / "oracle.go")],
                   check=True, env=env)
    texts = ["", " \t\n", "A\0中文😀B", "A^€@\r\nB"]
    basic = "@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà"
    texts += [basic, basic + "\f^{}\\[~]|€"]
    for n in [1, 152, 153, 159, 160, 161, 305, 306, 307, 1529, 1530, 1531, 1600, 1601]:
        texts.append("A" * n)
    for n in [76, 77, 79, 80, 81, 759, 760, 761, 764, 765, 766]:
        texts.append("^" * n)
    for n in [66, 67, 70, 71, 669, 670, 671]:
        texts.append("中" * n)
    for n in [33, 34, 35, 36, 329, 330, 331]:
        texts.append("😀" * n)
    texts += ["A" * 152 + "^B", "A" * 151 + "€AB", "中" * 66 + "😀中"]
    rng = random.Random(507)
    alphabet = "A@^€\0中😀\r\né[{}]"
    texts += ["".join(rng.choice(alphabet) for _ in range(rng.randrange(1, 300)))
              for _ in range(150)]
    cases = [{"number": number, "text": text, "ref": ref}
             for text in texts
             for number in ["10010", " +8613800138000 ", "12345678901234567890", "", "+", "123x"]
             for ref in [1, 255]]
    wire = "".join(json.dumps(c, ensure_ascii=False) + "\n" for c in cases).encode()
    expected = subprocess.run([gobin], input=wire, stdout=subprocess.PIPE, check=True).stdout.splitlines()
    got = subprocess.run([oracle], input=wire, stdout=subprocess.PIPE, check=True).stdout.splitlines()
    assert len(got) == len(expected) == len(cases)
    for i, (left, right) in enumerate(zip(expected, got)):
        assert json.loads(left) == json.loads(right), (i, cases[i], left, right)
    print(f"DOMAIN_ORACLE_PASS {len(cases)} exact Go/C PDU cases")


class FakeModem:
    def __init__(self, master):
        self.master = master
        self.stop = threading.Event()
        self.pin = self.sms = self.dial = self.hang = 0
        self.ready = False
        self.awaiting = False
        self.pending = bytearray()
        self.mode = "normal"
        self.mode_sms = 0
        self.call = "none"
        self.error = None

    def write(self, data):
        while data and not self.stop.is_set():
            n = os.write(self.master, data)
            data = data[n:]

    def result(self, line=b""):
        self.write(b"\r\n" + line + b"\r\nOK\r\n")

    def command(self, line):
        if line == b"AT+CPIN?":
            return self.result(b"+CPIN: READY" if self.ready else b"+CPIN: SIM PIN")
        if line.startswith(b"AT+CPIN="):
            self.pin += 1
            if line == b'AT+CPIN="1234"':
                self.ready = True
                return self.result()
            return self.write(b"\r\n+CME ERROR: 16\r\n")
        if line == b"AT+LABCOUNTS":
            return self.result(f"+LAB: {self.pin},{self.sms},{self.dial},{self.hang}".encode())
        if line.startswith(b"AT+LABMODE="):
            self.mode = line.split(b"=", 1)[1].decode()
            self.mode_sms = 0
            return self.result()
        if line.startswith(b"AT+LABCALL="):
            self.call = line.split(b"=", 1)[1].decode()
            return self.result()
        if line == b"AT+LABLONG":
            return self.result(b"A" * 65536)
        if line.startswith(b"AT+CMGS="):
            self.awaiting = True
            return self.write(b"\r\n> ")
        if line == b"AT+CLCC":
            lines = b'+CLCC: 1,0,0,1,0,"",129\r\n+CLCC: 2,0,0,1,0,"",129'
            if self.call != "none":
                state = 4 if self.call == "incoming" else 0
                lines += f'\r\n+CLCC: 3,1,{state},0,0,"10086",129'.encode()
            return self.result(lines)
        if line == b"ATA":
            self.call = "active"
            return self.result()
        if line == b"AT+CHUP":
            self.hang += 1
            self.call = "none"
            return self.result()
        if line.startswith(b"ATD"):
            self.dial += 1
            return self.write(b"\r\nERROR\r\n")
        if line == b"AT+QCCID":
            return self.result(b"+QCCID: 0000000000000000001")
        if line == b"AT+CIMI":
            return self.result(b"001010000000001")
        if line == b"AT+CNUM":
            return self.result(b'+CNUM: "LAB","+15550000001",145')
        if line == b"AT+CMGL=4":
            return self.result()
        return self.result()

    def run(self):
        try:
            while not self.stop.is_set():
                if not select.select([self.master], [], [], .1)[0]:
                    continue
                data = os.read(self.master, 8192)
                if not data:
                    continue
                for byte in data:
                    if self.awaiting:
                        if byte in (26, 27):
                            self.awaiting = False
                            if byte == 26:
                                assert self.pending and all(chr(c) in "0123456789ABCDEF" for c in self.pending)
                                self.sms += 1
                                self.mode_sms += 1
                                if self.mode == "okonly":
                                    self.result()
                                elif self.mode == "partial" and self.mode_sms == 2:
                                    self.write(b"\r\n+CMS ERROR: 500\r\n")
                                else:
                                    self.result(b"+CMGS: 1")
                            else:
                                self.result()
                            self.pending.clear()
                        else:
                            self.pending.append(byte)
                    elif byte == 13:
                        line = bytes(self.pending)
                        self.pending.clear()
                        self.command(line)
                    else:
                        self.pending.append(byte)
        except Exception as exc:
            self.error = exc


runtime = build("test_runtime", ["tests/test_domain_runtime.c", "tests/test_domain_support.c",
                "src/common.c", "src/crypto.c", "src/auth.c", "src/clock.c",
                "src/at.c", "src/pdu.c", "src/store.c", "src/outbox.c",
                "src/pin.c", "src/calls.c", "src/domain.c"], ["-lmbedcrypto"])
master, slave = pty.openpty()
tty.setraw(slave)
fake = FakeModem(master)
thread = threading.Thread(target=fake.run, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix="qdc-domain-runtime-") as data_dir:
        env = os.environ.copy()
        env["QDC_SYNTHETIC_PTY"] = "1"
        subprocess.run([runtime, data_dir, os.ttyname(slave)], check=True, env=env, timeout=45)
    assert fake.error is None, fake.error
finally:
    fake.stop.set()
    thread.join(timeout=2)
    os.close(master)
    os.close(slave)
print("DOMAIN_ALL_PASS local-only")
