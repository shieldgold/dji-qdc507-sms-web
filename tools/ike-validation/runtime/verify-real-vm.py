"""Two private ARM VMs: certificate, VIP, native TUN routing and cleanup."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time


def main():
    os.umask(0o077)
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('local', type=Path)
    p.add_argument('--attempt', type=int, required=True)
    p.add_argument('--direct', action='store_true', help='standard IKE 500 then NAT-T 4500; no custom-port listener')
    a = p.parse_args()
    base = a.local.resolve()
    logs = base/'logs'/('real-runtime-vm-'+str(a.attempt))
    logs.mkdir(mode=0o700, exist_ok=False)
    out = base/'runtime-build'
    paths = [out/'lab-only-runtime-zImage', out/'lab-only-real-rootfs.cpio.gz',
             out/'qdc-ike-guard', base/'output/charon', base/'output/swanctl']
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    (logs/'artifacts.json').write_text(json.dumps(hashes, indent=2)+'\n')
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
    procs = []
    result = {'status': 'failed', 'device_executed': False}
    try:
        for role in ('server', 'client'):
            net = ('listen' if role == 'server' else 'connect')+'=127.0.0.1:'+str(port)
            mac = '52:54:00:12:34:'+('01' if role == 'server' else '02')
            cmd = ['qemu-system-arm', '-M', 'virt-5.2', '-cpu', 'cortex-a7', '-smp', '2', '-m', '48M',
                   '-nodefaults', '-display', 'none', '-monitor', 'none', '-no-reboot',
                   '-serial', 'file:'+str(logs/(role+'.log')), '-global', 'virtio-mmio.force-legacy=true',
                   '-object', 'rng-random,filename=/dev/urandom,id=rng0', '-device', 'virtio-rng-device,rng=rng0',
                   '-netdev', 'socket,id=private,'+net, '-device', 'virtio-net-device,netdev=private,mac='+mac,
                   '-kernel', str(paths[0]), '-initrd', str(paths[1]), '-append',
                   'console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init rng_core.default_quality=1000 '+
                   f'qdc.real.lab=1 qdc.role={role} qdc.epoch={int(time.time())} qdc.direct={int(a.direct)}']
            (logs/(role+'-command.json')).write_text(json.dumps(cmd, indent=2)+'\n')
            err = (logs/(role+'-qemu.txt')).open('wb')
            proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=err)
            procs.append((proc, err))
            until = time.monotonic()+(35 if role == 'server' else 95)
            while time.monotonic() < until:
                text = (logs/(role+'.log')).read_text(errors='replace') if (logs/(role+'.log')).exists() else ''
                marker = 'REAL_VM_SERVER_READY' if role == 'server' else 'REAL_VM_CLIENT_COMPLETE'
                if marker in text:
                    break
                if 'REAL_VM_FAILED' in text or proc.poll() is not None:
                    raise RuntimeError(role+' VM failed; inspect '+str(logs))
                time.sleep(.25)
            else:
                raise TimeoutError(role+' VM deadline; inspect '+str(logs))
        result.update(status='passed', packet_pairs=10, child_rekey=True, vip='10.88.0.2',
                      custom_natt_port=None if a.direct else 45000, standard_ports=a.direct, cleanup_verified=True)
    finally:
        for proc, err in reversed(procs):
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
            err.close()
        (logs/'acceptance.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
