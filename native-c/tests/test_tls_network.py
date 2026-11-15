#!/usr/bin/env python3
"""All sockets bind/dial loopback; all credentials/certificates are synthetic."""
import argparse
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time


def run(*args, cwd):
    subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def exact(conn, length):
    out = b''
    while len(out) < length:
        block = conn.recv(length - len(out))
        if not block:
            raise RuntimeError('Unexpected local DNS EOF')
        out += block
    return out


def dns_fixture(binary):
    tcp = socket.socket(); tcp.bind(('127.0.0.1', 0)); tcp.listen(1)
    port = tcp.getsockname()[1]
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); udp.bind(('127.0.0.1', port))
    counts = {'udp': 0, 'tcp': 0}

    def udp_worker():
        query, peer = udp.recvfrom(512); counts['udp'] += 1
        udp.sendto(query[:2] + struct.pack('!HHHHH', 0x8380, 1, 0, 0, 0) + query[12:], peer)

    def tcp_worker():
        conn, _ = tcp.accept()
        with conn:
            query = exact(conn, struct.unpack('!H', exact(conn, 2))[0]); counts['tcp'] += 1
            alias = b'\x04edge\x05apple\x03com\0'
            cname = b'\xc0\x0c' + struct.pack('!HHIH', 5, 1, 60, len(alias)) + alias
            address = struct.pack('!H', 0xc000 | (len(query) + 12)) + struct.pack('!HHIH', 1, 1, 60, 4) + bytes([127, 0, 0, 7])
            response = query[:2] + struct.pack('!HHHHH', 0x8180, 1, 2, 0, 0) + query[12:] + cname + address
            framed = struct.pack('!H', len(response)) + response
            for i in range(0, len(framed), 3):
                conn.sendall(framed[i:i+3])

    threads = [threading.Thread(target=udp_worker, daemon=True), threading.Thread(target=tcp_worker, daemon=True)]
    for thread in threads:
        thread.start()
    try:
        result = subprocess.run([str(binary), 'dns', str(port)], capture_output=True, text=True, timeout=5)
        assert result.returncode == 0 and result.stdout.strip() == '127.0.0.7', result
        assert counts == {'udp': 1, 'tcp': 1}, counts
    finally:
        udp.close(); tcp.close()
    print('DNS loopback: UDP TC -> TCP, CNAME plus compressed target A, 3-byte TCP fragments PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix='qdc-tls-fixtures-') as directory:
        work = Path(directory)
        run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=Offline Test CA', cwd=work)
        run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'wrong.key', '-out', 'wrong.pem', '-days', '2', '-subj', '/CN=Untrusted Offline CA', cwd=work)
        run('openssl', 'req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-keyout', 'server.key', '-out', 'server.csr', '-subj', '/CN=localhost', cwd=work)
        (work / 'ext.cnf').write_text('subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n')
        run('openssl', 'x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'server.pem', '-days', '2', '-extfile', 'ext.cnf', cwd=work)
        server_js = Path(__file__).with_name('tls_server.cjs')
        cases = [('tls12', True, 'localhost', 'ca.pem', True, 200),
                 ('ok', True, 'localhost', 'ca.pem', True, 200),
                 ('failure', True, 'localhost', 'ca.pem', True, 410),
                 ('redirect', True, 'localhost', 'ca.pem', True, 302),
                 ('h1', False, 'localhost', 'ca.pem', True, 200),
                 ('h1', True, 'localhost', 'ca.pem', False, None),
                 ('ok', True, 'wrong.invalid', 'ca.pem', False, None),
                 ('ok', True, 'localhost', 'wrong.pem', False, None),
                 ('bad-h1', False, 'localhost', 'ca.pem', False, None),
                 ('bad-h1-lf', False, 'localhost', 'ca.pem', False, None),
                 ('reset', True, 'localhost', 'ca.pem', False, None),
                 ('oversize', True, 'localhost', 'ca.pem', False, None),
                 ('delay', True, 'localhost', 'ca.pem', False, None)]
        for mode, h2, name, ca, success, status in cases:
            server = subprocess.Popen(['node', str(server_js), str(work/'server.key'), str(work/'server.pem'), mode], stdout=subprocess.PIPE, text=True)
            try:
                port = str(json.loads(server.stdout.readline())['port'])
                path = '/3/device/' + 'a' * 64 if h2 else '/library/test/success.html'
                before = time.monotonic()
                result = subprocess.run([str(binary), 'tls', port, str(work/ca), name, str(int(h2)), path, '250' if mode == 'delay' else '2500'], capture_output=True, text=True, timeout=5)
                elapsed = time.monotonic() - before
                assert (result.returncode == 0) == success, (mode, result)
                if status:
                    assert 'status=' + str(status) in result.stdout, result
                if mode == 'delay':
                    assert elapsed < 1.5, elapsed
                print(f'TLS loopback {mode}/h2={h2}/{name}/{ca}: PASS')
            finally:
                server.terminate(); server.wait(timeout=3)
    dns_fixture(binary)


if __name__ == '__main__':
    main()
