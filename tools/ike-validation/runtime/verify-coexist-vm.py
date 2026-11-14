"""Computer-only ARM coexistence cases on a private QEMU socket network."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

CASES = ('normal_30s', 'wrong_guardian', 'no_go', 'http_failure',
         'sms_restart', 'memory_abort', 'signal_abort')


def validate_case(case, text):
    required = ('COEXIST_VM_CASE_PASS case='+case,
                'COEXIST_VM_NETWORK_UNCHANGED', 'COEXIST_VM_SMS_ALIVE',
                'COEXIST_VM_VPN_CLEANED', 'COEXIST_VM_CLOCK_ORDER_OK')
    for marker in required:
        if marker not in text:
            raise ValueError('missing acceptance evidence: '+marker)
    if 'COEXIST_VM_FAILED' in text or 'RECOVERY_REQUIRED' in text:
        raise ValueError('guest reported failure or incomplete recovery')
    match = re.search(r'COEXIST_VM_FIRST_HTTP epoch=(\d+)', text)
    if not match or not 315532800 <= int(match[1]) < 315533000:
        raise ValueError('SMS first HTTP did not use the restored guest clock')
    if case in ('wrong_guardian', 'no_go'):
        if 'OWNED label=charon' in text or 'CONNECTED expected_vip=' in text:
            raise ValueError('unarmed/refused request started the VPN')
        expected = 'PREFLIGHT_COEXIST_REFUSED' if case == 'wrong_guardian' else 'LOGGER_NOT_ARMED'
        if expected not in text:
            raise ValueError('missing expected refusal')
    elif case == 'normal_30s':
        match = re.search(r'COEXIST_TRAFFIC_COMPLETE seconds=([0-9.]+) ping_pairs=10 http_success=10', text)
        if not match or float(match[1]) < 30:
            raise ValueError('full 30-second traffic window not demonstrated')
        if len(re.findall(r'COEXIST_PAIR index=\d+ ping=1 http_status=200', text)) != 10:
            raise ValueError('expected ten distinct encrypted ping/HTTP pairs')
        for marker in ('SMAPS label=charon ', 'SMAPS label=smsweb ',
                       'COEXIST_CLI_RELEASED no_swanctl_process=1',
                       'COEXIST_POST_SMS_OK',
                       'COEXIST_COMPLETE rc=0 children_reaped=1'):
            if marker not in text:
                raise ValueError('missing successful coexistence evidence: '+marker)
    else:
        if 'COEXIST_TRAFFIC_COMPLETE' in text or 'COEXIST_COMPLETE rc=0' in text:
            raise ValueError('injected failure was accepted as success')
        expected = {'http_failure': 'STOP coexist_sms_http',
                    'sms_restart': 'STOP sms_or_supervisor_identity_changed',
                    'memory_abort': 'STOP memory ',
                    'signal_abort': 'COEXIST_VM_INJECT signal_abort'}[case]
        if expected not in text:
            raise ValueError('missing expected stop: '+expected)


def run_case(base, out, logs, case, hashes):
    logs.mkdir(mode=0o700)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    kernel = base/'runtime-build/lab-only-runtime-zImage'
    initrd = out/'lab-only-coexist-rootfs.cpio.gz'
    processes = []
    result = {'case': case, 'status': 'failed', 'device_executed': False}
    try:
        for role in ('server', 'client'):
            network = ('listen' if role == 'server' else 'connect')+'=127.0.0.1:'+str(port)
            command = ['qemu-system-arm', '-M', 'virt-5.2', '-cpu', 'cortex-a7',
                       '-smp', '2', '-m', '48M', '-nodefaults', '-display', 'none',
                       '-monitor', 'none', '-no-reboot', '-serial', 'file:'+str(logs/(role+'.log')),
                       '-global', 'virtio-mmio.force-legacy=true',
                       '-object', 'rng-random,filename=/dev/urandom,id=rng0',
                       '-device', 'virtio-rng-device,rng=rng0',
                       '-netdev', 'socket,id=private,'+network,
                       '-device', 'virtio-net-device,netdev=private,mac=52:54:00:12:35:'+('01' if role == 'server' else '02'),
                       '-kernel', str(kernel), '-initrd', str(initrd), '-append',
                       'console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init rng_core.default_quality=1000 '+
                       f'qdc.coexist.lab=1 qdc.role={role} qdc.case={case} qdc.epoch={int(time.time())}']
            (logs/(role+'-command.json')).write_text(json.dumps(command, indent=2)+'\n')
            error = (logs/(role+'-qemu.txt')).open('wb')
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error)
            processes.append((process, error))
            deadline = time.monotonic()+(35 if role == 'server' else 95)
            while time.monotonic() < deadline:
                path = logs/(role+'.log')
                text = path.read_text(errors='replace') if path.exists() else ''
                marker = 'COEXIST_VM_SERVER_READY' if role == 'server' else 'COEXIST_VM_CASE_PASS case='+case
                if marker in text:
                    if role == 'client':
                        validate_case(case, text)
                    break
                if 'COEXIST_VM_FAILED' in text or process.poll() is not None:
                    raise RuntimeError(role+' guest failed; inspect '+str(logs))
                time.sleep(.25)
            else:
                raise TimeoutError(role+' guest deadline; inspect '+str(logs))
        result.update(status='passed', private_socket_only=True, standard_ike_ports=True,
                      sms_restored=True, clock_order_verified=True, cleanup_verified=True)
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        for process, error in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
            error.close()
        result['artifact_sha256'] = hashes
        (logs/'acceptance.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('local', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--attempt', type=int, required=True)
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    if args.attempt < 1:
        parser.error('--attempt must be positive')
    base, out = args.local.resolve(), args.out.resolve()
    logs = out/'logs'/('arm-coexist-'+str(args.attempt))
    logs.parent.mkdir(mode=0o700, exist_ok=True)
    logs.mkdir(mode=0o700, exist_ok=False)
    paths = [base/'runtime-build/lab-only-runtime-zImage', out/'lab-only-coexist-rootfs.cpio.gz',
             out/'qdc-ike-guard', out/'lab-only-coexist-fixture', base/'output/charon', base/'output/swanctl']
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    (logs/'artifacts.json').write_text(json.dumps(hashes, indent=2)+'\n')
    results = []
    aggregate = {'status': 'failed', 'device_executed': False, 'guard_sha256': hashes[str(out/'qdc-ike-guard')],
                 'cases': [], 'results': results}
    try:
        for case in args.case or CASES:
            result = run_case(base, out, logs/case, case, hashes)
            results.append(result)
            aggregate['cases'].append(case)
            print('CASE_PASS '+case, flush=True)
        aggregate['status'] = 'passed'
    finally:
        (logs/'acceptance.json').write_text(json.dumps(aggregate, indent=2)+'\n')
    print(json.dumps({key: aggregate[key] for key in ('status', 'device_executed', 'guard_sha256', 'cases')}))


if __name__ == '__main__':
    main()
