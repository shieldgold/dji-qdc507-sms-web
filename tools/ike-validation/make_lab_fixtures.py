"""Generate disposable lab identities only, never reads user credentials."""
from pathlib import Path
import subprocess
import sys

p = Path(sys.argv[1]).resolve()
p.mkdir(mode=0o700, parents=True, exist_ok=True)
if any(p.iterdir()): raise SystemExit('fixture directory must be empty')
def run(*args):
    subprocess.run(['openssl', *args], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
run('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-x509', '-days', '2', '-sha256',
    '-subj', '/CN=QDC OFFLINE LAB CA', '-keyout', str(p/'ca.key'), '-out', str(p/'ca.pem'),
    '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
for role in ['server', 'client']:
    run('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-subj', '/CN='+role+'.lab',
        '-keyout', str(p/(role+'.key')), '-out', str(p/(role+'.csr')))
    ext = p/(role+'.ext')
    ext.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth,clientAuth\nsubjectAltName=DNS:'+role+'.lab\n')
    run('x509', '-req', '-in', str(p/(role+'.csr')), '-CA', str(p/'ca.pem'), '-CAkey', str(p/'ca.key'),
        '-CAcreateserial', '-days', '2', '-sha256', '-extfile', str(ext), '-out', str(p/(role+'.pem')))
for f in p.iterdir(): f.chmod(0o600)
print('Disposable RSA-2048 lab certificates created; no user credentials accessed')
