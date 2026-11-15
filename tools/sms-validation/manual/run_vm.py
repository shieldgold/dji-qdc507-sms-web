"""Isolated ARM migration/soak acceptance; never connects USB or real peers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import time

MANUAL_CASES = (
    'early_handoff_retains_install', 'running_c_to_original_retains_install',
    'missing_soak_proof_rolls_back', 'committed_boot_session_still_rolls_back',
    'installed_symlink_refused', 'owned_vpn_guard_retired_before_restore',
    'unknown_overlay_retained', 'failed_soak_auto_rolls_back',
    'preparation_timeout_reclaims_partial_upload',
    'low_restore_memory_keeps_supervisor_stopped',
)
SOAK_CASES = ('normal', 'wrong_baseline', 'no_go', 'http_failure', 'signal_abort')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_manual(text):
    if 'SMS_SWAP_VM_FAILED' in text or 'Kernel panic' in text:
        raise ValueError('manual guest failed')
    for case in MANUAL_CASES:
        if 'SMS_MANUAL_VM_CASE ' + case + ' PASS' not in text:
            raise ValueError('missing manual case ' + case)
    duration = re.search(r'SMS_MANUAL_VM_PREP_ELAPSED seconds=([0-9.]+)', text)
    if not duration or not 119 <= float(duration[1]) < 128:
        raise ValueError('real 120-second preparation deadline not demonstrated')
    if 'SMS_MANUAL_VM_PASS cases=10 original_unchanged=1 install_retained=1 device_executed=0' not in text:
        raise ValueError('missing manual final proof')
    return {'status': 'passed', 'cases': list(MANUAL_CASES), 'preparation_real_seconds': float(duration[1])}


def validate_soak(case, text):
    for marker in ('SMS_SOAK_VM_PASS case=' + case, 'SMS_SOAK_VM_NETWORK_UNCHANGED',
                   'SMS_SOAK_VM_PRODUCTION_PROOF_NOT_FAKED', 'MANUAL_ROLLBACK_COMPLETE source_retained=1'):
        if marker not in text:
            raise ValueError('missing soak evidence: ' + marker)
    if 'SMS_SOAK_VM_FAILED' in text or 'Kernel panic' in text or 'RECOVERY_REQUIRED' in text:
        raise ValueError('soak guest failed or recovery incomplete')
    if case == 'normal':
        match = re.search(r'SOAK_TRAFFIC_COMPLETE real_seconds=([0-9.]+) window_seconds=([0-9.]+) ping_pairs=(\d+) http_success=(\d+) reconnects=3 rekeys=1', text)
        if not match or float(match[1]) < 120 or float(match[2]) < 3600 or int(match[3]) < 300 or match[3] != match[4]:
            raise ValueError('scaled long window/traffic incomplete')
        if re.findall(r'SOAK_RECONNECTED index=(\d+) ', text) != ['1', '2', '3']:
            raise ValueError('not three distinct reconnects')
        if 'SOAK_REKEY_COMPLETE inbound_and_outbound_spi_changed=1 protected_ping=1' not in text:
            raise ValueError('no real ESP rekey proof')
        if 'SOAK_COMPLETE rc=0 children_reaped=1' not in text:
            raise ValueError('missing complete cleanup')
        return {'status': 'passed', 'scaled_functional_only': True, 'clock_rate': 30,
                'real_seconds': float(match[1]), 'window_seconds': float(match[2]), 'pairs': int(match[3]),
                'reconnects': 3, 'rekeys': 1, 'production_hour_not_claimed': True}
    if 'SOAK_TRAFFIC_COMPLETE' in text or 'SOAK_COMPLETE rc=0' in text:
        raise ValueError('injected failure accepted as success')
    if case in ('wrong_baseline', 'no_go') and 'OWNED label=charon' in text:
        raise ValueError('refused/unarmed case started VPN')
    expected = {'wrong_baseline': 'SOAK_REFUSED stage=pre_c_baseline_proof',
                'no_go': 'LOGGER_NOT_ARMED', 'http_failure': 'HTTP_PROBE status=503',
                'signal_abort': 'SOAK_COMPLETE rc=1 children_reaped=1'}[case]
    if expected not in text:
        raise ValueError('missing expected stop: ' + expected)
    return {'status': 'passed', 'expected_stop': case, 'scaled_functional_only': True}


def command(kernel, initrd, log, role=None, port=None, case=None, epoch=None):
    cmd = ['qemu-system-arm', '-M', 'virt-5.2', '-cpu', 'cortex-a7', '-m', '48M',
           '-nodefaults', '-display', 'none', '-monitor', 'none', '-no-reboot', '-serial', 'file:' + str(log)]
    flags = 'console=ttyAMA0 loglevel=3 panic=1 rdinit=/init '
    if role:
        network = ('listen' if role == 'server' else 'connect') + '=127.0.0.1:' + str(port)
        cmd += ['-smp', '2', '-global', 'virtio-mmio.force-legacy=true',
                '-object', 'rng-random,filename=/dev/urandom,id=rng0', '-device', 'virtio-rng-device,rng=rng0',
                '-netdev', 'socket,id=private,' + network,
                '-device', 'virtio-net-device,netdev=private,mac=52:54:00:12:39:' + ('01' if role == 'server' else '02')]
        if not isinstance(epoch, int) or epoch < 1:
            raise ValueError('sealed offline certificate epoch required')
        flags += f'maxcpus=1 rng_core.default_quality=1000 qdc.coexist.lab=1 qdc.role={role} qdc.case=normal_30s qdc.soak.case={case} qdc.epoch={epoch}'
    else:
        cmd += ['-smp', '1', '-nic', 'none']
        flags += 'qdc.sms.swap.lab=1'
    return cmd + ['-kernel', str(kernel), '-initrd', str(initrd), '-append', flags]


def launch(cmd, error_path):
    error = error_path.open('xb')
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error)
    return proc, error


def wait_marker(proc, log, marker, seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        text = log.read_text(errors='replace') if log.exists() else ''
        if marker in text:
            return text
        if any(x in text for x in ('SMS_SWAP_VM_FAILED', 'SMS_SOAK_VM_FAILED', 'COEXIST_VM_FAILED', 'Kernel panic')) or proc.poll() is not None:
            raise RuntimeError('guest failed: ' + str(log))
        time.sleep(.15)
    raise TimeoutError('guest deadline: ' + str(log))


def stop(procs):
    for proc, error in reversed(procs):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        error.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('local', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--only', choices=('all', 'manual', *SOAK_CASES), default='all')
    a = p.parse_args()
    out, local = a.output.resolve(), a.local.resolve()
    build = json.loads((out / 'build.json').read_text())
    source = Path(__file__).resolve().parents[2]
    for name, digest in build['source_sha256'].items():
        if sha(source / name) != digest:
            p.error('source changed after build: ' + name)
    for name, meta in build['artifacts'].items():
        if sha(out / name) != meta['sha256']:
            p.error('production artifact changed after build')
    for name, digest in build['initrd_sha256'].items():
        if sha(out / name) != digest:
            p.error('initrd changed after build')
    if sha(out / 'lab-only-soak-clock30') != build['lab_clock30_sha256']:
        p.error('test artifact changed after build')
    kernel = local / 'runtime-build/lab-only-runtime-zImage'
    logs = out / 'logs'
    logs.mkdir(mode=0o700, exist_ok=True)
    selection = (['manual', *SOAK_CASES] if a.only == 'all' else [a.only])
    for case in selection:
        directory = logs / case
        directory.mkdir(mode=0o700)
        procs, result = [], {'status': 'failed', 'case': case, 'device_executed': False,
                             'kernel_sha256': sha(kernel), 'production_artifacts': build['artifacts']}
        try:
            if case == 'manual':
                cmd = command(kernel, out / 'lab-only-manual-rootfs.cpio.gz', directory / 'manual.log')
                (directory / 'command.json').write_text(json.dumps(cmd, indent=2) + '\n')
                proc, error = launch(cmd, directory / 'qemu.log')
                procs.append((proc, error))
                text = wait_marker(proc, directory / 'manual.log', 'SMS_MANUAL_VM_PASS', 190)
                result.update(validate_manual(text))
            else:
                with socket.socket() as reserve:
                    reserve.bind(('127.0.0.1', 0))
                    port = reserve.getsockname()[1]
                for role in ('server', 'client'):
                    log = directory / (role + '.log')
                    cmd = command(kernel, out / 'lab-only-soak-rootfs.cpio.gz', log, role, port, case, build['lab_epoch'])
                    (directory / (role + '-command.json')).write_text(json.dumps(cmd, indent=2) + '\n')
                    proc, error = launch(cmd, directory / (role + '-qemu.log'))
                    procs.append((proc, error))
                    marker = 'COEXIST_VM_SERVER_READY' if role == 'server' else 'SMS_SOAK_VM_PASS case=' + case
                    text = wait_marker(proc, log, marker, 40 if role == 'server' else 210)
                result.update(validate_soak(case, text))
        except Exception as exc:
            result['error'] = str(exc)
            raise
        finally:
            stop(procs)
            (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({'case': case, 'status': result['status'], 'device_executed': False}), flush=True)
    expected = ['manual', *SOAK_CASES]
    results = [json.loads((logs / x / 'result.json').read_text()) for x in expected if (logs / x / 'result.json').is_file()]
    if len(results) == len(expected) and all(x['status'] == 'passed' for x in results):
        (out / 'acceptance.json').write_text(json.dumps({'schema': 1, 'status': 'passed', 'device_executed': False,
            'hardware_hour_not_claimed': True, 'clock_rate': {'production': 1, 'lab_soak': 30},
            'build_sha256': sha(out / 'build.json'), 'cases': expected, 'results': results}, indent=2) + '\n')
        print('MANUAL_MIGRATION_ARM_ACCEPTANCE_PASSED', flush=True)


if __name__ == '__main__':
    main()
