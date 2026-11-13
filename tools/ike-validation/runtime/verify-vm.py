"""Run only a private ARM VM on this computer; no USB passthrough or networking."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

CASES = ['no_go', 'normal_cleanup', 'streamed_app_logs', 'early_exit_cleanup', 'kmsg_abort_cleanup',
         'cli_timeout_cleanup', 'runtime_memory_abort', 'blocked_output_deadline', 'low_memory_refusal',
         'supervisor_auto_resume', 'supervisor_manual_resume', 'original_network_unchanged']


def accepted(text):
    return ('RUNTIME_VM_COMPLETE' in text and 'RUNTIME_VM_FAILED' not in text
            and all('CASE_PASS '+case+'\n' in text for case in CASES))


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('local', type=Path)
    parser.add_argument('--attempt', type=int, required=True)
    args = parser.parse_args()
    if args.attempt < 1:
        parser.error('positive attempt required')
    base = args.local.resolve()
    out = base/'runtime-build'
    logs = base/'logs'/f'runtime-tools-vm-{args.attempt}'
    logs.mkdir(mode=0o700, exist_ok=False)
    paths = [out/'lab-only-runtime-zImage', out/'lab-only-runtime-rootfs.cpio.gz',
             out/'qdc-ike-guard', base/'output/charon', base/'output/swanctl']
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    (logs/'artifacts.json').write_text(json.dumps(hashes, indent=2)+'\n')
    cmd = ['qemu-system-arm', '-M', 'virt-5.2', '-cpu', 'cortex-a7', '-smp', '2', '-m', '48M',
           '-nodefaults', '-display', 'none', '-monitor', 'none', '-serial', 'file:'+str(logs/'guest.log'),
           '-no-reboot', '-global', 'virtio-mmio.force-legacy=true',
           '-object', 'rng-random,filename=/dev/urandom,id=rng0', '-device', 'virtio-rng-device,rng=rng0',
           '-kernel', str(paths[0]), '-initrd', str(paths[1]),
           '-append', 'console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init rng_core.default_quality=1000 qdc.runtime.lab=1']
    (logs/'command.json').write_text(json.dumps(cmd, indent=2)+'\n')
    result = {'status': 'failed', 'device_executed': False, 'cases': CASES}
    with (logs/'qemu-stderr.txt').open('wb') as stderr:
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=stderr)
        try:
            until = time.monotonic()+90
            while time.monotonic() < until:
                text = (logs/'guest.log').read_text(errors='replace') if (logs/'guest.log').exists() else ''
                if accepted(text):
                    result['status'] = 'passed'
                    break
                if 'RUNTIME_VM_FAILED' in text or proc.poll() is not None:
                    raise RuntimeError('VM failed; inspect '+str(logs/'guest.log'))
                time.sleep(.25)
            else:
                raise TimeoutError('VM deadline; inspect '+str(logs/'guest.log'))
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            (logs/'acceptance.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
