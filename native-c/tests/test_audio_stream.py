#!/usr/bin/env python3
"""Host-only fake PCM helper; no modem, sound device, or real call."""
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

driver = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="qdc-audio-stream-") as tmp:
    root = pathlib.Path(tmp)
    (root / "voice-runtime").mkdir()
    helper = root / "voice-runtime/network-pcm-bridge"
    helper.write_text("""#!/usr/bin/env python3
import os, signal, sys, time
if '--network-route-session' in sys.argv:
    os.write(1, b'VoLTE route session active\\n' + b'x' * 200000)
    while True: time.sleep(1)
else:
    os.write(2, b'y' * 100000)
    while True:
        b = os.read(0, 320)
        if not b: break
        offset = 0
        while offset < len(b): offset += os.write(1, b[offset:])
""")
    helper.chmod(0o700)
    (root / "pcm").touch()
    (root / "ready").write_text("12 987\n")
    proc = root / "proc/12"
    proc.mkdir(parents=True)
    (proc / "stat").write_text("12 (fake calibrator) S " + "0 " * 18 + "987 0\n")
    (proc / "cmdline").write_bytes(b"/usr/bin/alsaucm_test\0")
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    p = subprocess.Popen([str(driver), tmp, str(port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    def cmd(value):
        p.stdin.write(value + "\n")
        p.stdin.flush()
        line = p.stdout.readline()
        assert line, (p.poll(), p.stderr.read() if p.poll() is not None else "driver stopped responding")
        return json.loads(line)
    def connect(ticket, suffix=b"\n"):
        c = socket.create_connection(("127.0.0.1", port), timeout=3)
        c.sendall(b"PCM1 " + ticket.encode() + suffix)
        return c
    try:
        assert p.stdout.readline().strip() == "READY"
        ticket = cmd("ticket")["ticket"]
        with connect(ticket, b"\r\n") as c:
            assert c.recv(3) == b""
        with connect(ticket) as c:
            assert c.recv(3) == b"OK\n"
            c.sendall(bytes(320))
            cmd("callb")
            assert c.recv(3) == b""
        assert cmd("stats")["hangups"] == 0
        cmd("active")
        ticket = cmd("ticket")["ticket"]
        with connect(ticket) as c:
            assert c.recv(3) == b"OK\n"
            # Give the fake helpers time to create/drain their bounded logs.
            time.sleep(0.3)
            payload = bytes(range(256)) + bytes(64)
            c.sendall(payload[:17]); c.sendall(payload[17:])
            received = bytearray()
            while len(received) < 320:
                received.extend(c.recv(320 - len(received)))
            assert received == payload
            status = cmd("status")
            assert status["state"] == "streaming" and status["up_bytes"] >= 320 and status["down_bytes"] >= 320
            cmd("none")
            assert c.recv(1) == b""
        assert cmd("stats")["hangups"] == 0
        assert (root / "voice.log").stat().st_size <= 65536
        assert (root / "voice.log").stat().st_mode & 0o777 == 0o600
        p.stdin.write("quit\n"); p.stdin.flush()
        assert p.wait(timeout=10) == 0
    finally:
        if p.poll() is None:
            p.terminate()
            try: p.wait(timeout=10)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
    print("audio stream/ticket/call identity/log cleanup: PASS")
