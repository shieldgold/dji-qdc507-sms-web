"""Prepare protected, per-file real-connection cards; never opens USB or runs VPN."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
from prepare import build_bundle, probe_input, PINS, write_private


def connection_files(params, credentials):
    if set(params) != {'relay', 'server_id', 'client_id', 'target', 'vip'}:
        raise ValueError('exact public connection parameter set required')
    for key in ('relay', 'target', 'vip'):
        ip = ipaddress.IPv4Address(params[key])
        if ip.is_unspecified or ip.is_loopback or ip.is_multicast or int(ip) >> 24 >= 224:
            raise ValueError('unicast IPv4 required')
    if params['target'] == params['vip']:
        raise ValueError('target and VIP must differ')
    for key in ('client_id', 'server_id'):
        if not re.fullmatch(r'[A-Za-z0-9@._-]{1,200}', params[key]):
            raise ValueError('identity contains unsafe characters')
    if credentials.is_symlink() or credentials.stat().st_mode & 0o077:
        raise ValueError('private non-symlink credential directory required')
    files = {}
    for name in ('client.key', 'client.pem', 'ca.pem'):
        path = credentials/name
        if path.is_symlink() or not path.is_file() or path.stat().st_mode & 0o077:
            raise ValueError('private regular credential files required')
        data = path.read_bytes()
        if not 0 < len(data) <= 65536:
            raise ValueError('credential size outside staging bound')
        files[name] = data
    subprocess.run(['openssl', 'verify', '-CAfile', str(credentials/'ca.pem'), str(credentials/'client.pem')],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    key_public = subprocess.check_output(['openssl', 'pkey', '-in', str(credentials/'client.key'), '-pubout', '-passin', 'pass:'],
                                         stderr=subprocess.DEVNULL)
    cert_public = subprocess.check_output(['openssl', 'x509', '-in', str(credentials/'client.pem'), '-pubkey', '-noout'],
                                          stderr=subprocess.DEVNULL)
    if key_public != cert_public:
        raise ValueError('client certificate and private key do not match')
    files['swanctl.conf'] = ('''connections {
 qdc-test {
  version = 2
  remote_addrs = %(relay)s
  local_port = 4500
  remote_port = 45000
  proposals = aes256-sha256-modp2048
  encap = yes
  mobike = no
  rekey_time = 0
  reauth_time = 0
  vips = 0.0.0.0
  local {
   auth = pubkey
   id = %(client_id)s
   certs = client.pem
  }
  remote {
   auth = pubkey
   id = %(server_id)s
  }
  children {
   qdc-test {
    local_ts = dynamic
    remote_ts = %(target)s/32
    esp_proposals = aes256-sha256
    rekey_time = 0
    start_action = none
   }
  }
 }
}
secrets {
 rsa-test {
  file = client.key
 }
}
''' % params).encode()
    return {'target': params['target'], 'vip': params['vip'], 'files': files}


def main():
    os.umask(0o077)
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('local', type=Path)
    p.add_argument('--tun-local', type=Path, required=True)
    p.add_argument('--credentials', type=Path, required=True)
    p.add_argument('--params', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--run', required=True)
    a = p.parse_args()
    build = json.loads((a.local/'runtime-build/build.json').read_text())
    sources = {n: a.local/'output'/n for n in ('charon', 'swanctl')}
    sources['tun.ko'] = a.tun_local/'output/tun.ko'
    sources['qdc-ike-guard'] = a.local/'runtime-build/qdc-ike-guard'
    sources['qdc-tun-probe'], digest = probe_input(a.local, a.tun_local, build, True)
    expected = {**PINS, 'qdc-ike-guard': build['guard_sha256'], 'qdc-tun-probe': digest}
    connect = connection_files(json.loads(a.params.read_text()), a.credentials)
    manifest = build_bundle(sources, expected, a.out.absolute(), a.run, connect)
    write_private(a.out/'build.json', (json.dumps(build, indent=2)+'\n').encode())
    write_private(a.out/'public-connection.json', a.params.read_bytes())
    print(json.dumps({'prepared': str(a.out), 'scope': manifest['scope'], 'device_executed': False}))


if __name__ == '__main__':
    main()
