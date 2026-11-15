"""Verify clock-restore failure in a private ARM 3.18 VM; never accesses hardware."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import time


CASES = ('failure', 'delegate-success', 'delegate-failure')


def validate(text, case='failure'):
    required = ('CLOCK_FAILURE_KERNEL release=3.18.44',
                'CLOCK_FAILURE_SUPERVISOR identity=exact_busybox_argv',
                'RESUME_GUARD_READY', 'restore_clock=1',
                'CLOCK_FAILURE_CLEANUP owned_supervisor_group_removed=1', 'CLOCK_FAILURE_PASS')
    if case not in CASES or any(marker not in text for marker in required):
        raise ValueError('missing exact identity/capability/stopped-state evidence')
    if any(marker in text for marker in ('RESUME_SENT', 'CLOCK_FAILURE_FAILED',
                                         'Kernel panic', 'Unable to handle kernel')):
        raise ValueError('clock recovery resumed the supervisor or fixture failed')
    if case == 'delegate-success':
        match = re.search(r'CLOCK_FAILURE_OBSERVED case=delegate-success rc=0 supervisor_state=T resume_sent=0 delegated=1 epoch=(\d+)', text)
        if not match or not 315532800 <= int(match[1]) < 315532900:
            raise ValueError('delegation did not restore the real guest clock while stopped')
        proof = 'RESUME_DELEGATED verify_supervisor_before_cont=1'
        if 'RECOVERY_REQUIRED' in text or 'CLOCK_RESTORED' not in text or proof not in text or text.index('CLOCK_RESTORED') > text.index(proof):
            raise ValueError('missing ordered clock/delegation proof')
    else:
        required = ('CLOCK_FAILURE_CAPS uid=0 effective=0 permitted=0 inheritable=0 bounding=0 cap_sys_time=25',
                    'RECOVERY_REQUIRED clock_restore: Operation not permitted',
                    'CLOCK_FAILURE_OBSERVED case='+case+' rc=1 supervisor_state=T resume_sent=0 delegated=0')
        if any(marker not in text for marker in required) or 'RESUME_DELEGATED' in text or 'CLOCK_RESTORED' in text:
            raise ValueError('failed clock restoration claimed recovery or resumed the supervisor')


def command(kernel, initrd, serial, case='failure'):
    if case not in CASES:
        raise ValueError('invalid clock-recovery case')
    return ['qemu-system-arm', '-M', 'virt-5.2', '-cpu', 'cortex-a7', '-smp', '2', '-m', '48M',
            '-nodefaults', '-display', 'none', '-monitor', 'none', '-serial', 'file:'+str(serial),
            '-no-reboot', '-nic', 'none', '-global', 'virtio-mmio.force-legacy=true',
            '-kernel', str(kernel), '-initrd', str(initrd), '-append',
            'console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init qdc.clockfailure.lab=1 qdc.clockfailure.case='+case]


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--guard', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--case', choices=CASES, default='failure')
    args = parser.parse_args()
    kernel, guard, out = args.kernel.resolve(), args.guard.resolve(), args.out.resolve()
    if not kernel.is_file() or not guard.is_file():
        parser.error('explicit kernel/guard files required')
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    source = Path(__file__).resolve().parent
    build = ['docker', 'run', '--rm', '--network', 'none', '--platform', 'linux/amd64',
             '-v', str(source)+':/src:ro', '-v', str(guard)+':/input/qdc-ike-guard:ro',
             '-v', str(out)+':/out', '-v', 'qdc507-ike-build-v1:/work:ro',
             'qdc507-ike-builder:local', 'sh', '/src/build.sh']
    (out/'build-command.json').write_text(json.dumps(build, indent=2)+'\n')
    with (out/'build.log').open('wb') as log:
        subprocess.run(build, check=True, stdout=log, stderr=subprocess.STDOUT)
    initrd = out/'lab-only-clock-failure-rootfs.cpio.gz'
    cmd = command(kernel, initrd, out/'guest.log', args.case)
    (out/'command.json').write_text(json.dumps(cmd, indent=2)+'\n')
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    result = {'status': 'failed', 'case': args.case, 'device_executed': False, 'network': 'none',
              'guard_sha256': sha(guard), 'kernel_sha256': sha(kernel),
              'rootfs_sha256': sha(initrd),
              'launcher_sha256': sha(out/'lab-only-clock-failure-launcher'),
              'source_sha256': {name: sha(source/name) for name in ('launcher.c', 'init.sh', 'build.sh', 'run.py')}}
    with (out/'qemu-stderr.log').open('wb') as error:
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error)
        try:
            deadline = time.monotonic()+45
            while time.monotonic() < deadline:
                text = (out/'guest.log').read_text(errors='replace') if (out/'guest.log').exists() else ''
                if 'CLOCK_FAILURE_PASS' in text or 'CLOCK_FAILURE_FAILED' in text or proc.poll() is not None:
                    break
                time.sleep(.25)
            else:
                raise TimeoutError('clock-failure guest deadline')
            validate(text, args.case)
            result['status'] = 'passed'
        except Exception as failure:
            result['error'] = str(failure)
            raise
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait()
            (out/'acceptance.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
