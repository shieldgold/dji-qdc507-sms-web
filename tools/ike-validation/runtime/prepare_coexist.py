"""Computer-only SMS/VPN preparation and identity binding. No device or network I/O."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import gzip
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import stat
from prepare import build_bundle, checked_id, probe_input, PINS, RAM_CHECK, write_private
from prepare_connect import connection_files

VM_CASES = {'normal_30s', 'wrong_guardian', 'no_go', 'http_failure',
            'sms_restart', 'memory_abort', 'signal_abort'}
ARTIFACTS = {'charon', 'swanctl', 'tun.ko', 'qdc-tun-probe', 'qdc-ike-guard',
             'strongswan.conf', 'client.key', 'client.pem', 'ca.pem', 'swanctl.conf'}


def rootfs_guard_hash(path):
    """Read bounded newc initrd data; never extract or execute guest files."""
    try:
        with gzip.open(path, 'rb') as stream:
            data = stream.read(32*1024*1024+1)
    except (OSError, EOFError) as exc:
        raise ValueError('unreadable or truncated lab initrd') from exc
    if len(data) > 32*1024*1024:
        raise ValueError('lab initrd exceeds validation bound')
    offset, found, trailer = 0, [], False
    while offset+110 <= len(data):
        header = data[offset:offset+110]
        if header[:6] != b'070701' or not re.fullmatch(b'[0-9A-Fa-f]{104}', header[6:]):
            raise ValueError('invalid newc header')
        fields = [int(header[6+n*8:14+n*8], 16) for n in range(13)]
        mode, uid, gid, size, namesize = fields[1], fields[2], fields[3], fields[6], fields[11]
        begin = offset+110; end = begin+namesize
        if not 1 <= namesize <= 4096 or end > len(data) or data[end-1] != 0 or b'\0' in data[begin:end-1]:
            raise ValueError('invalid newc pathname')
        name = data[begin:end-1]
        if name.startswith(b'./'): name = name[2:]
        body = (end+3)&~3; stop = body+size
        if stop > len(data):
            raise ValueError('truncated newc data')
        offset = (stop+3)&~3
        if name == b'TRAILER!!!':
            if size or any(data[offset:]): raise ValueError('unexpected data after newc trailer')
            trailer = True; break
        if name == b'bin/qdc-ike-guard':
            if not stat.S_ISREG(mode) or uid or gid or not 0 < size <= 192*1024:
                raise ValueError('embedded guard must be a bounded root-owned regular file')
            found.append(hashlib.sha256(data[body:stop]).hexdigest())
    if not trailer or len(found) != 1:
        raise ValueError('exactly one embedded guard and complete trailer required')
    return found[0]


def checked_connection(connection):
    if set(connection) != {'target', 'vip', 'transport', 'peer_address'} or connection['transport'] != 'direct':
        raise ValueError('exact direct connection metadata required')
    for name in ('target', 'vip', 'peer_address'):
        value = connection[name]
        if not isinstance(value, str):
            raise ValueError('canonical IPv4 text required')
        ip = ipaddress.IPv4Address(value)
        if str(ip) != value or int(ip) >> 24 == 0 or ip.is_loopback or int(ip) >> 24 >= 224:
            raise ValueError('canonical unicast IPv4 required')
    if len({connection[k] for k in ('target', 'vip', 'peer_address')}) != 3:
        raise ValueError('distinct target, VIP and physical IKE endpoint required')


def control_bytes(run, snapshot, now=None):
    """Bind only public, fresh identities; the native helper checks them again."""
    checked_id(run+'_COEX')
    expected = {'schema', 'observed_at', 'boot_id', 'http_address', 'original_sms_gone',
                'supervisor', 'original_sms', 'guardian'}
    if not isinstance(snapshot, dict) or set(snapshot) != expected or type(snapshot['schema']) is not int or snapshot['schema'] != 1 or snapshot['original_sms_gone'] is not True:
        raise ValueError('exact post-pause identity snapshot required')
    try:
        observed = datetime.fromisoformat(snapshot['observed_at'].replace('Z', '+00:00'))
        if observed.utcoffset() is None:
            raise ValueError('timezone required')
        age = ((now or datetime.now(timezone.utc))-observed).total_seconds()
    except (TypeError, AttributeError) as exc:
        raise ValueError('UTC observation timestamp required') from exc
    if not -5 <= age <= 60:
        raise ValueError('identity observation must be within 60 seconds, at most 5 seconds ahead')
    if not isinstance(snapshot['boot_id'], str) or not re.fullmatch(r'[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}', snapshot['boot_id']):
        raise ValueError('canonical boot ID required')
    if not isinstance(snapshot['http_address'], str):
        raise ValueError('canonical IPv4 text required')
    try:
        ip = ipaddress.IPv4Address(snapshot['http_address'])
    except ipaddress.AddressValueError as exc:
        raise ValueError('local RFC1918 or loopback IPv4 required') from exc
    if not any(ip in ipaddress.IPv4Network(n) for n in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '127.0.0.0/8')):
        raise ValueError('local RFC1918 or loopback IPv4 required')
    sup, sms, guardian = (snapshot[k] for k in ('supervisor', 'original_sms', 'guardian'))
    for proc in (sup, sms, guardian):
        if type(proc.get('pid')) is not int or not 1 <= proc['pid'] <= 2147483647 or type(proc.get('start')) is not int or not 1 <= proc['start'] <= 18446744073709551615:
            raise ValueError('positive PID and unsigned starttime required')
    if len({p['pid'] for p in (sup, sms, guardian)}) != 3:
        raise ValueError('three distinct process identities required')
    if sup != {'pid': sup['pid'], 'start': sup['start'], 'exe': '/bin/busybox', 'state': 'T',
               'argv': ['/bin/busybox', '/bin/sh', '/data/smsweb/start.sh', 'supervise']}:
        raise ValueError('stopped original SMS supervisor required')
    if sms != {'pid': sms['pid'], 'start': sms['start'], 'exe': '/data/smsweb/smsweb'}:
        raise ValueError('original SMS executable identity required')
    helper = f'/run/qdc-tun-{run}/qdc-ike-guard'
    if guardian != {'pid': guardian['pid'], 'start': guardian['start'], 'exe': helper,
                    'argv': [helper, 'resume-clock', str(sup['pid']), str(sup['start']), '300']}:
        raise ValueError('same-run clock-restoring 300-second guardian required')
    return (f'QDC_COEXIST_V1\nRUN {run}\nBOOT {snapshot["boot_id"]}\nTOKEN {run}_COEX\n'
            f'SUP {sup["pid"]} {sup["start"]}\nSMS {sms["pid"]} {sms["start"]}\n'
            f'RESUME {guardian["pid"]} {guardian["start"]}\nHTTP {ip}\nSECONDS 30\n').encode()


def bind_bundle(bundle, snapshot, destination):
    bundle, destination = bundle.absolute(), destination.absolute()
    if bundle.is_symlink() or bundle.stat().st_mode & 0o077:
        raise ValueError('private non-symlink bundle required')
    allowed = {'payload', 'cards', 'manifest.json', 'READ-ME-FIRST.txt', 'build.json',
               'public-connection.json', 'compact-probe-vm.json', 'coexist-vm.json'}
    for path in bundle.rglob('*'):
        parts = path.relative_to(bundle).parts
        if path.is_symlink() or not (path.is_dir() or path.is_file()) or parts[0] not in allowed or len(parts) > 2:
            raise ValueError('unexpected bundle entry or symlink')
        if (len(parts) == 1 and path.is_dir() != (parts[0] in ('cards', 'payload'))) or (len(parts) == 2 and (parts[0] not in ('cards', 'payload') or not path.is_file())):
            raise ValueError('unexpected bundle directory')
        if path.stat().st_mode & 0o077:
            raise ValueError('bundle contains non-private entries')
    manifest = json.loads((bundle/'manifest.json').read_text())
    if manifest.get('mode') != 'coexist' or manifest.get('execution_blocked') is not True or manifest.get('preflight_kib') != 12288:
        raise ValueError('unbound 12 MiB coexist bundle required')
    run = checked_id(manifest['run_id']); control = control_bytes(run, snapshot)
    checked_connection(manifest['connection'])
    if set(manifest['artifacts']) != ARTIFACTS:
        raise ValueError('exact coexist artifact set required')
    if manifest['ram_root'] != '/run/qdc-tun-'+run:
        raise ValueError('run root mismatch')
    expected_parts = set()
    for name, meta in manifest['artifacts'].items():
        if set(meta) != {'bytes', 'sha256', 'parts'} or type(meta['bytes']) is not int or not 0 < meta['bytes'] <= 4*1048576 or not re.fullmatch('[0-9a-f]{64}', meta['sha256']):
            raise ValueError('invalid artifact metadata')
        if meta['parts'] != [f'{name}.part{i:03d}' for i in range((meta['bytes']+1048575)//1048576)]:
            raise ValueError('exact ordered artifact part names required')
        data = bytearray()
        for part in meta['parts']:
            if not re.fullmatch(re.escape(name)+r'\.part[0-9]{3}', part):
                raise ValueError('unsafe artifact part name')
            path = bundle/'payload'/part
            if path.stat().st_size > 1048576:
                raise ValueError('artifact part too large')
            data.extend(path.read_bytes()); expected_parts.add(part)
        if len(data) != meta['bytes'] or hashlib.sha256(data).hexdigest() != meta['sha256']:
            raise ValueError('payload size/hash mismatch')
    if {p.name for p in (bundle/'payload').iterdir()} != expected_parts:
        raise ValueError('unexpected payload entry')
    actual_cards = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (bundle/'cards').iterdir() if p.is_file()}
    if actual_cards != manifest['cards_sha256']:
        raise ValueError('stage card hash mismatch')
    shutil.copytree(bundle, destination, copy_function=shutil.copy2)
    for path in (destination/'cards').glob('*.json'):
        card = json.loads(path.read_text())
        if card['op'] == 'push':
            card['local'] = str(destination/'payload'/Path(card['local']).name)
            path.write_text(json.dumps(card)+'\n')
    root = manifest['ram_root']; cards = destination/'cards'
    part = 'coexist.control.part000'; digest = hashlib.sha256(control).hexdigest()
    write_private(destination/'payload'/part, control)
    requests = [
        {'op': 'push', 'id': '29-0-control-push', 'seconds': 25,
         'local': str(destination/'payload'/part), 'remote': root+'/'+part},
        {'op': 'shell', 'id': '29-1-control-assemble', 'seconds': 10,
         'command': 'set -eu\n'+RAM_CHECK+f'cd {root}\ntest "$(cat RUN_ID)" = {shlex.quote(run)}\n'
                    'test ! -e coexist.control\ntest ! -L coexist.control\n'
                    f"printf '%s\\n' '{digest}  {part}' | sha256sum -c -\n"
                    f'mv {part} coexist.control\nchmod 0400 coexist.control\n'},
        {'op': 'shell', 'id': '40-coexist', 'seconds': 90, 'ready': 'LOGGER_READY '+run+'_COEX',
         'command': f'{root}/qdc-ike-guard coexist {root} {run}_COEX 30 {manifest["connection"]["target"]} {manifest["connection"]["vip"]}'}]
    for card in requests:
        write_private(cards/(card['id']+'.json'), (json.dumps(card)+'\n').encode())
    at = manifest['stages'].index('30-load-tun'); manifest['stages'][at:at] = ['29-0-control-push', '29-1-control-assemble']
    at = manifest['stages'].index('80-unload-tun'); manifest['stages'].insert(at, '40-coexist')
    manifest.update(execution_blocked=False, device_executed=False, binding_observed_at=snapshot['observed_at'],
                    binding_sha256=hashlib.sha256(json.dumps(snapshot, sort_keys=True).encode()).hexdigest())
    manifest['artifacts']['coexist.control'] = {'bytes': len(control), 'sha256': digest, 'parts': [part]}
    manifest['cards_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in cards.glob('*.json')}
    (destination/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
    return manifest


def prepare_bundle(a):
    build = json.loads((a.guard_build/'build.json').read_text())
    digest = build['guard_sha256']
    receipt_path = a.guard_build/('coexist-'+digest+'.json')
    receipt = json.loads(receipt_path.read_text())
    if receipt.get('schema') != 2 or receipt.get('status') != 'passed' or receipt.get('guard_sha256') != digest or not VM_CASES <= set(receipt.get('cases', [])) or receipt.get('regression_cases') != 20 or receipt.get('embedded_guard_sha256') != {'coexist': digest, 'regression': digest}:
        raise ValueError('matching completed ARM coexist and 20-case regression proof required')
    old = json.loads((a.local/'runtime-build/build.json').read_text())
    probe, probe_hash = probe_input(a.local, a.tun_local, old, True)
    sources = {n: a.local/'output'/n for n in ('charon', 'swanctl')}
    sources.update({'tun.ko': a.tun_local/'output/tun.ko', 'qdc-ike-guard': a.guard_build/'qdc-ike-guard', 'qdc-tun-probe': probe})
    pins = {**PINS, 'qdc-ike-guard': digest, 'qdc-tun-probe': probe_hash}
    connect = connection_files(json.loads(a.params.read_text()), a.credentials, a.direct)
    connect['mode'] = 'coexist'
    manifest = build_bundle(sources, pins, a.out.absolute(), a.run, connect, low_copy=True)
    for name, data in [('build.json', (a.guard_build/'build.json').read_bytes()), ('coexist-vm.json', receipt_path.read_bytes()),
                       ('public-connection.json', a.params.read_bytes())]:
        write_private(a.out/name, data)
    return manifest


def publish_receipt(local, out, coexist_logs, regression_logs):
    """Re-read final guest evidence and bind it to the current exact binaries."""
    here = Path(__file__).resolve().parent
    def module(filename):
        spec = importlib.util.spec_from_file_location('_qdc_validation_'+filename, here/filename)
        result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
        return result
    coexist_vm, regression_vm = module('verify-coexist-vm.py'), module('verify-vm.py')
    build = json.loads((out/'build.json').read_text())
    digest = hashlib.sha256((out/'qdc-ike-guard').read_bytes()).hexdigest()
    if build['guard_sha256'] != digest:
        raise ValueError('guard build metadata/hash mismatch')
    for name, expected in build['source_sha256'].items():
        if hashlib.sha256((here.parent.parent/name).read_bytes()).hexdigest() != expected:
            raise ValueError('guard sources changed after build')
    paths = [local/'runtime-build/lab-only-runtime-zImage', out/'lab-only-coexist-rootfs.cpio.gz',
             out/'qdc-ike-guard', out/'lab-only-coexist-fixture', local/'output/charon', local/'output/swanctl']
    hashes = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    embedded = {'coexist': rootfs_guard_hash(out/'lab-only-coexist-rootfs.cpio.gz'),
                'regression': rootfs_guard_hash(out/'lab-only-runtime-rootfs.cpio.gz')}
    if embedded != {'coexist': digest, 'regression': digest}:
        raise ValueError('VM initrd embeds another guard; rebuild both lab rootfs images')
    aggregate = json.loads((coexist_logs/'acceptance.json').read_text())
    if aggregate.get('status') != 'passed' or aggregate.get('guard_sha256') != digest or set(aggregate.get('cases', [])) != VM_CASES or len(aggregate.get('results', [])) != len(VM_CASES):
        raise ValueError('all seven final ARM coexist cases required')
    if {r.get('case') for r in aggregate['results']} != VM_CASES:
        raise ValueError('distinct coexist case records required')
    for case in VM_CASES:
        record = json.loads((coexist_logs/case/'acceptance.json').read_text())
        if record.get('case') != case or record.get('status') != 'passed' or record.get('artifact_sha256') != hashes:
            raise ValueError('coexist case artifact/hash mismatch')
        coexist_vm.validate_case(case, (coexist_logs/case/'client.log').read_text(errors='replace'))
    regression = json.loads((regression_logs/'acceptance.json').read_text())
    old_hashes = json.loads((regression_logs/'artifacts.json').read_text())
    if regression.get('status') != 'passed' or set(regression.get('cases', [])) != set(regression_vm.CASES) or not regression_vm.accepted((regression_logs/'guest.log').read_text(errors='replace')):
        raise ValueError('all twenty final ARM regression cases required')
    regression_paths = [out/'lab-only-runtime-zImage', out/'lab-only-runtime-rootfs.cpio.gz',
                        out/'qdc-ike-guard', local/'output/charon', local/'output/swanctl', out/'qdc-tun-probe-musl']
    expected_regression = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in regression_paths}
    if old_hashes != expected_regression:
        raise ValueError('exact current six regression artifacts required')
    receipt = {'schema': 2, 'status': 'passed', 'device_executed': False, 'guard_sha256': digest,
               'embedded_guard_sha256': embedded,
               'cases': sorted(VM_CASES), 'regression_cases': len(regression_vm.CASES),
               'coexist_logs': str(coexist_logs), 'regression_logs': str(regression_logs),
               'artifact_sha256': hashes, 'sms_fixture': 'synthetic HTTP service; not real SMS RAM or business acceptance'}
    target = out/('coexist-'+digest+'.json')
    write_private(target, (json.dumps(receipt, indent=2)+'\n').encode())
    return target


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__); sub = parser.add_subparsers(dest='mode', required=True)
    prep = sub.add_parser('prepare', help='create unbound computer-only package')
    prep.add_argument('local', type=Path)
    for name in ('tun-local', 'guard-build', 'credentials', 'params', 'out'):
        prep.add_argument('--'+name, type=Path, required=True)
    prep.add_argument('--run', required=True); prep.add_argument('--direct', type=ipaddress.IPv4Address, required=True)
    bind = sub.add_parser('bind', help='offline: create fresh single-step cards; never execute')
    bind.add_argument('bundle', type=Path); bind.add_argument('--snapshot', type=Path, required=True); bind.add_argument('--out', type=Path, required=True)
    accept = sub.add_parser('accept', help='offline: revalidate exact final ARM evidence and publish matching receipt')
    accept.add_argument('local', type=Path)
    for name in ('out', 'coexist-logs', 'regression-logs'):
        accept.add_argument('--'+name, type=Path, required=True)
    a = parser.parse_args()
    if a.mode == 'accept':
        path = publish_receipt(a.local.resolve(), a.out.resolve(), a.coexist_logs.resolve(), a.regression_logs.resolve())
        print(json.dumps({'receipt': str(path), 'device_executed': False})); return
    manifest = prepare_bundle(a) if a.mode == 'prepare' else bind_bundle(a.bundle, json.loads(a.snapshot.read_text()), a.out)
    print(json.dumps({'prepared': str(a.out.absolute()), 'device_executed': False,
                      'execution_blocked': manifest['execution_blocked'], 'coexist_seconds': 30}))


if __name__ == '__main__': main()
