#!/usr/bin/env python3
"""Synthetic APNs protocol peer on loopback, never Apple or a real token."""
import argparse
import base64
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(*args, cwd):
    subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    binary = parser.parse_args().binary.resolve()
    with tempfile.TemporaryDirectory(prefix='qdc-apns-fixtures-') as directory:
        base = Path(directory)
        run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=Offline APNs CA', cwd=base)
        run('openssl', 'req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-keyout', 'server.key', '-out', 'server.csr', '-subj', '/CN=localhost', cwd=base)
        (base/'ext.cnf').write_text('subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n')
        run('openssl', 'x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'server.pem', '-days', '2', '-extfile', 'ext.cnf', cwd=base)
        run('openssl', 'genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', 'synthetic.p8', cwd=base)
        run('openssl', 'pkey', '-in', 'synthetic.p8', '-pubout', '-out', 'public.pem', cwd=base)

        def config(name):
            path = base/name; path.mkdir()
            (path/'push-apns.json').write_text(json.dumps(dict(key_id='ABC1234567', team_id='TEAM123456', topic='com.shield.modulephone', environment='development')))
            shutil.copyfile(base/'synthetic.p8', path/'push-apns.p8'); shutil.copyfile(base/'ca.pem', path/'push-ca.pem')
            for file in path.iterdir(): file.chmod(0o600)
            return path

        path = config('jwt')
        output = subprocess.run([str(binary), 'jwt', str(path)], capture_output=True, text=True, timeout=5)
        assert output.returncode == 0, 'JWT signing/cache helper failed'
        tokens = output.stdout.splitlines(); assert len(tokens) == 4
        for token in tokens:
            head, claims, raw = token.split('.')
            decode = lambda s: base64.urlsafe_b64decode(s + '=' * (-len(s) % 4))
            assert json.loads(decode(head)) == {'alg': 'ES256', 'kid': 'ABC1234567'}
            assert json.loads(decode(claims))['iss'] == 'TEAM123456'
            sig = decode(raw); assert len(sig) == 64
            # Independent OpenSSL DER verification of the JWT's P1363 R||S.
            def integer(block):
                block = block.lstrip(b'\0') or b'\0'
                if block[0] & 128: block = b'\0' + block
                return b'\x02' + bytes([len(block)]) + block
            der = integer(sig[:32]) + integer(sig[32:]); der = b'\x30' + bytes([len(der)]) + der
            (base/'sig.der').write_bytes(der); (base/'signed.txt').write_bytes((head+'.'+claims).encode())
            run('openssl', 'dgst', '-sha256', '-verify', 'public.pem', '-signature', 'sig.der', 'signed.txt', cwd=base)
        print('APNs crypto: independent ES256 signature + 50-minute cache + rollback invalidation PASS')
        for mode, kind, accepted, queued in [('ok','sms',1,0),('ok','test',1,0),('ok','call',1,0),
            ('BadDeviceToken','sms',0,0),('Unregistered','sms',0,0),('InvalidProviderToken','sms',0,0),
            ('TopicDisallowed','sms',0,0),('ExpiredProviderToken','sms',0,1),('TooManyRequests','sms',0,1),('ServiceUnavailable','sms',0,1)]:
            path = config(mode+'-'+kind)
            server = subprocess.Popen(['node', str(Path(__file__).with_name('apns_server.cjs')), str(base/'server.key'), str(base/'server.pem'), str(base/'public.pem'), mode, kind], stdout=subprocess.PIPE, text=True)
            try:
                port = str(json.loads(server.stdout.readline())['port'])
                output = subprocess.run([str(binary),'sender',str(path),port,kind], capture_output=True, text=True, timeout=8)
                assert output.returncode == 0, 'Synthetic sender helper failed'
                status = json.loads(output.stdout)
                assert status['accepted'] == accepted and status['queued'] == queued, (mode,kind,status)
                print(f'APNs loopback {mode}/{kind}: PASS')
            finally:
                server.terminate(); server.wait(timeout=3)
        invalid = config('invalid-ca'); (invalid/'push-ca.pem').write_text('invalid CA')
        output = subprocess.run([str(binary),'inspect',str(invalid)], capture_output=True,text=True,check=True)
        assert not json.loads(output.stdout)['configured'], 'Invalid CA must fail configuration before registration'
        invalid = config('invalid-mode'); (invalid/'push-apns.p8').chmod(0o644)
        output = subprocess.run([str(binary),'inspect',str(invalid)], capture_output=True,text=True,check=True)
        assert not json.loads(output.stdout)['configured'], 'World-readable key was accepted'
        invalid = config('empty-key'); (invalid/'push-apns.p8').write_bytes(b'')
        output = subprocess.run([str(binary),'inspect',str(invalid)], capture_output=True,text=True,check=True)
        assert not json.loads(output.stdout)['configured'], 'Empty key was accepted'
        print('APNs private configuration: invalid CA and world-readable key rejected PASS')


if __name__ == '__main__':
    main()
