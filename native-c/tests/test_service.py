#!/usr/bin/env python3
"""Complete C service against the existing synthetic modem, on host loopback."""
import argparse
import ctypes
import hashlib
import http.client
import json
import os
from pathlib import Path
import pty
import select
import socket
import subprocess
import tempfile
import threading
import time
import tty

ROOT = Path(__file__).resolve().parents[2]


class Fake(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint) for name in ("commands", "polls", "submitted", "cancelled", "unlocks")] + [(name, ctypes.c_int) for name in ("pin_locked", "awaiting_pdu", "messages")]


def run(binary, scenario):
    with tempfile.TemporaryDirectory(prefix="qdc-c-service-") as tmp:
        work = Path(tmp)
        dylib = work / "fake-modem.so"
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", str(ROOT / "tools/sms-validation/fake_modem.c"), "-o", str(dylib)], check=True)
        lib = ctypes.CDLL(str(dylib))
        for name in ("sms_fake_command", "sms_fake_control"):
            fn = getattr(lib, name)
            fn.restype = ctypes.c_size_t
        lib.sms_fake_command.argtypes = [ctypes.POINTER(Fake), ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
        lib.sms_fake_control.argtypes = [ctypes.POINTER(Fake), ctypes.c_ubyte, ctypes.c_void_p, ctypes.c_size_t]
        state = Fake(pin_locked=scenario == "pin", messages=1)
        master, slave = pty.openpty()
        tty.setraw(slave)
        stopped = threading.Event()
        errors = []
        card = ["0000000000000000001"]

        def modem():
            pending = bytearray()
            output = ctypes.create_string_buffer(16384)
            try:
                while not stopped.is_set():
                    if not select.select([master], [], [], .1)[0]:
                        continue
                    data = os.read(master, 8192)
                    for byte in data:
                        n = 0
                        if state.awaiting_pdu:
                            if byte in (0x1a, 0x1b):
                                n = lib.sms_fake_control(ctypes.byref(state), byte, output, len(output))
                                pending.clear()
                        elif byte == 13:
                            command = bytes(pending)
                            pending.clear()
                            n = lib.sms_fake_command(ctypes.byref(state), command, output, len(output))
                            if command in (b"AT+QCCID", b"AT+CCID", b"AT+ICCID"):
                                prefix = command.split(b"+")[1]
                                response = b"\r\n+" + prefix + b": " + card[0].encode() + b"\r\nOK\r\n"
                                os.write(master, response)
                                n = 0
                            elif command == b"ATI":
                                os.write(master, b"\r\nRevision: SYNTHETIC-C-LAB\r\nOK\r\n")
                                n = 0
                        elif byte != 10:
                            pending.append(byte)
                        if n:
                            os.write(master, output.raw[:n])
            except OSError as error:
                if not stopped.is_set(): errors.append(str(error))

        thread = threading.Thread(target=modem)
        thread.start()
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        data_dir = work / "data"
        data_dir.mkdir(mode=0o700)
        seen = set()
        cookie = ""
        log = (work / "service.log").open("wb")
        p = subprocess.Popen([str(binary), "-at", os.ttyname(slave), "-listen", "127.0.0.1", "-port", str(port), "-data", str(data_dir), "-poll", "100ms"], stdin=subprocess.DEVNULL, stdout=log, stderr=log)

        def request(method, path, body=None, status=200, use_cookie=True):
            nonlocal cookie
            payload = json.dumps(body, ensure_ascii=False).encode() if body is not None else None
            headers = {"Content-Type": "application/json"}
            if cookie and use_cookie: headers["Cookie"] = cookie
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            try:
                connection.request(method, path, payload, headers)
                response = connection.getresponse()
                raw = response.read()
                assert response.status == status, (method, path, response.status, raw[:512])
                assert int(response.getheader("Content-Length")) == len(raw)
                set_cookie = response.getheader("Set-Cookie")
                if set_cookie: cookie = set_cookie.split(";", 1)[0]
                seen.add((method, path.split("?", 1)[0]))
                return json.loads(raw) if "application/json" in response.getheader("Content-Type", "") else raw
            finally:
                connection.close()

        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    request("GET", "/api/auth/status")
                    break
                except (OSError, http.client.HTTPException):
                    assert p.poll() is None, (p.returncode, (work / "service.log").read_text())
                    assert time.monotonic() < deadline
                    time.sleep(.05)
            for name, path in (("index.html", "/"), ("style.css", "/style.css"), ("model.js", "/model.js"), ("app.js", "/app.js")):
                assert request("GET", path) == (ROOT / "web" / name).read_bytes()
            request("GET", "/api/state", status=401, use_cookie=False)
            assert request("GET", "/api/sim/binding")["iccid"] == card[0]
            password = "合成\0lab-only"
            request("POST", "/api/auth/setup", {"password": password, "confirm": password, "remember": False})
            assert cookie.startswith("smsweb_token=")
            assert request("GET", "/api/auth/status")["authenticated"]
            request("POST", "/api/sim/retry", {"confirmed": False}, status=400)
            if scenario == "pin":
                assert request("GET", "/api/sim")["state"] == "SIM PIN"
            request("POST", "/api/sim/unlock", {"pin": "1234", "request_id": "c-lab-pin-0001", "expected_iccid": card[0]})
            assert request("GET", "/api/sim")["state"] == "READY"
            deadline = time.monotonic() + 10
            while True:
                messages = request("GET", "/api/state")["messages"]
                if any(m.get("code") == "654321" for m in messages): break
                assert time.monotonic() < deadline
                time.sleep(.05)
            send = {"request_id": "c-lab-sms-0001", "number": "+15550000001", "text": "A\0中文😀B"}
            request("POST", "/api/send", send, status=202)
            deadline = time.monotonic() + 10
            while True:
                message = request("GET", "/api/send/status?request_id=c-lab-sms-0001")
                if message["send_state"] == "submitted": break
                assert time.monotonic() < deadline, message
                time.sleep(.05)
            assert message["body"] == send["text"]
            request("POST", "/api/send", send, status=202)
            assert state.submitted == 1
            request("GET", "/api/overview")
            request("POST", "/api/diagnostics", {})
            request("GET", "/api/calls")
            request("POST", "/api/calls/audio", {}, status=409)
            request("POST", "/api/calls/dial", {"number": "+15550000001", "request_id": "c-lab-call-0001"}, status=409)
            for action in ("answer", "hangup", "dtmf"):
                request("POST", "/api/calls/" + action, {"id": "stale-call-id", "digit": "1"}, status=409)
            request("GET", "/api/push/status")
            request("POST", "/api/push/register", {"expected_iccid": card[0], "environment": "development", "device_id": "c-synthetic-lab", "enabled": False, "calls_enabled": False})
            request("POST", "/api/push/test", {}, status=409)
            request("POST", "/api/time", {"unix_ms": int(time.time() * 1000)})
            backup = request("GET", "/api/export")
            assert backup["version"] == 1
            imported = [{"key": f"c-import-{i}", "index": 99, "sender": "synthetic", "body": "中" * 500 + "\0", "time": "2026-11-15 00:00:00", "status": "已收", "direction": "in"} for i in range(30)]
            restored = request("POST", "/api/restore", {"version": 1, "messages": imported})
            assert restored["added"] == 30
            assert request("POST", "/api/restore", {"version": 1, "messages": imported})["added"] == 0
            after = request("GET", "/api/export")["messages"]
            assert all(m["index"] == 0 for m in after if m["key"].startswith("c-import-"))
            request("POST", "/api/delete", {"key": "c-import-0"})
            request("POST", "/api/maintenance/rollback", {}, status=400)
            ticket = request("POST", "/api/auth/browser/ticket", {})
            request("POST", "/api/auth/browser/redeem", {"ticket": ticket["ticket"], "expected_iccid": card[0]}, use_cookie=False)
            request("POST", "/api/auth/browser/redeem", {"ticket": ticket["ticket"], "expected_iccid": card[0]}, status=401, use_cookie=False)
            request("POST", "/api/auth/change", {"old": password, "password": "next-lab-only", "confirm": "next-lab-only"})
            request("POST", "/api/auth/logout", {})
            request("POST", "/api/auth/login", {"password": "next-lab-only", "remember": True, "expected_iccid": card[0]})
            old_card = card[0]
            card[0] = "0000000000000000002"
            request("POST", "/api/sim/unlock", {"pin": "1234", "request_id": "c-stale-pin-0001", "expected_iccid": old_card}, status=409)
            request("GET", "/api/state", status=401)
            assert not request("GET", "/api/auth/status")["registered"]
            card[0] = old_card
            assert not request("GET", "/api/auth/status")["authenticated"]
            request("POST", "/api/auth/login", {"password": "next-lab-only", "expected_iccid": old_card})
            assert state.submitted == 1 and (state.unlocks == 1 if scenario == "pin" else state.unlocks == 0)
            expected = {(r["method"], r["path"]) for r in json.loads((ROOT / "native-c/compat/routes.json").read_text())["routes"]}
            assert not (expected - seen), expected - seen
            assert not errors, errors
            p.terminate()
            assert p.wait(timeout=15) == 0
            text = (work / "service.log").read_text(errors="replace")
            assert not any(x in text for x in ("AddressSanitizer", "runtime error:", "LeakSanitizer")), text[-2000:]
            print(json.dumps({"status": "passed", "scenario": scenario, "api_routes": len(expected), "submitted": state.submitted, "unlocks": state.unlocks, "frontend_bytes_unchanged": True, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}))
        finally:
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=15)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
            stopped.set(); thread.join(timeout=2)
            os.close(master); os.close(slave); log.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--scenario", choices=("ready", "pin"), default="ready")
    args = parser.parse_args()
    run(args.binary.resolve(), args.scenario)
