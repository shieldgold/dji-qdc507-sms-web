"""Full-system ARM QEMU guests; their private Ethernet link has no Internet."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('local', type=Path)
parser.add_argument('--auth', choices=['pubkey', 'psk'], default='pubkey')
parser.add_argument('--threads', type=int, choices=[8, 16], default=16)
parser.add_argument('--attempt', type=int, default=1)
args = parser.parse_args()
base = args.local.resolve()
case = f'{args.auth}-threads{args.threads}'
if args.attempt < 1: parser.error('attempt must be positive')
logs = base/'logs'/(case+f'-attempt{args.attempt}')
logs.mkdir(mode=0o700, exist_ok=False)
artifacts = {n: hashlib.sha256((base/'output'/n).read_bytes()).hexdigest()
             for n in ['charon', 'swanctl', 'lab-only-zImage', 'lab-only-rootfs.cpio.gz']}
(logs/'artifacts.json').write_text(json.dumps(artifacts, indent=2)+'\n')
with socket.socket() as s:
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]

def command(role):
    mode = f'listen=127.0.0.1:{port}' if role == 'server' else f'connect=127.0.0.1:{port}'
    mac = '52:54:00:12:34:01' if role == 'server' else '52:54:00:12:34:02'
    # Old GIC discovery needs a multi-CPU controller; keep only CPU0 online.
    return ['qemu-system-arm', '-M', 'virt-5.2', '-cpu', 'cortex-a7', '-smp', '2', '-m', '48M',
        '-nodefaults', '-display', 'none', '-monitor', 'none', '-serial', 'file:'+str(logs/(role+'.log')),
        '-no-reboot', '-global', 'virtio-mmio.force-legacy=true',
        '-object', 'rng-random,filename=/dev/urandom,id=rng0', '-device', 'virtio-rng-device,rng=rng0',
        '-netdev', 'socket,id=private,'+mode, '-device', 'virtio-net-device,netdev=private,mac='+mac,
        '-kernel', str(base/'output/lab-only-zImage'), '-initrd', str(base/'output/lab-only-rootfs.cpio.gz'),
        '-append', f'console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init rng_core.default_quality=1000 qdc.lab=1 qdc.role={role} qdc.threads={args.threads if role == "client" else 16} qdc.auth={args.auth} qdc.epoch={int(time.time())}']

processes = []
try:
    for role in ['server', 'client']:
        cmd = command(role)
        (logs/(role+'-command.json')).write_text(json.dumps(cmd, indent=2)+'\n')
        stderr = (logs/(role+'-qemu.txt')).open('wb')
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=stderr)
        processes.append((role, p, stderr))
        if role == 'server':
            end = time.monotonic()+40
            while time.monotonic() < end:
                if p.poll() is not None: raise RuntimeError('server VM exited before READY')
                text = (logs/'server.log').read_text(errors='replace') if (logs/'server.log').exists() else ''
                if 'LAB_READY role=server' in text: break
                if 'LAB_CLIENT_FAILED' in text or 'Kernel panic' in text: raise RuntimeError('server guest failed')
                time.sleep(.2)
            else: raise RuntimeError('server READY timeout')
    end = time.monotonic()+100
    while time.monotonic() < end:
        text = (logs/'client.log').read_text(errors='replace') if (logs/'client.log').exists() else ''
        if 'LAB_CLIENT_FAILED' in text or 'Kernel panic' in text: raise RuntimeError('client guest failed')
        if 'LAB_CLIENT_COMPLETE' in text:
            if '100 packets transmitted, 100 packets received, 0% packet loss' not in text: raise RuntimeError('packet acceptance missing')
            (logs/'acceptance.json').write_text(json.dumps({'status':'passed', 'auth':args.auth, 'worker_threads':args.threads, 'packet_pairs':100}, indent=2)+'\n')
            print('LAB_ACCEPTED', case, flush=True)
            break
        if processes[-1][1].poll() is not None: raise RuntimeError('client VM exited before completion')
        time.sleep(.5)
    else: raise RuntimeError('client completion timeout')
except Exception as exc:
    (logs/'acceptance.json').write_text(json.dumps({'status':'failed', 'error':str(exc)}, indent=2)+'\n')
    raise
finally:
    for role, p, stderr in reversed(processes):
        if p.poll() is None:
            p.terminate()
            try: p.wait(timeout=3)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
        stderr.close()
