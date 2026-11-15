"""Run the real Linux PTY FD/reader regressions on a disposable ARM 3.18 VM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

TESTS=('TestOpenModemCloseReleasesBlockedPTY','TestModemSetupFailureReleasesPTYDescriptor')

def validate_log(text,variant):
    if 'SMS_PTY_KERNEL release=3.18.44' not in text or 'SMS_PTY_COMPLETE' not in text:
        raise ValueError('no completed ARM 3.18.44 PTY run')
    if any(x in text for x in ('SMS_PTY_INIT_FAILED','Kernel panic','SIGSEGV','panic: test timed out')):
        raise ValueError('unrelated runtime/fixture failure')
    if variant=='old':
        for name in TESTS:
            if not re.search(r'^--- FAIL: '+name+r' \(',text,re.M):raise ValueError('old binary did not reproduce both failures')
        for marker in ('closing modem blocked behind its kernel PTY read','failed setup blocked while closing its PTY reader','SMS_PTY_EXIT code=1'):
            if marker not in text:raise ValueError('old binary failed for an unexpected reason')
        status='expected_red'
    elif variant=='new':
        for name in TESTS:
            if not re.search(r'^--- PASS: '+name+r' \(',text,re.M):raise ValueError('new binary did not pass both PTY tests')
        if 'SMS_PTY_EXIT code=0' not in text or '--- FAIL:' in text:raise ValueError('new binary reported a test failure')
        status='passed'
    else:raise ValueError('invalid variant')
    return {'status':status,'variant':variant,'device_executed':False,'tests':list(TESTS),'real_pty':True,'kernel':'3.18.44'}

def qemu_command(kernel,initrd,serial):
    return ['qemu-system-arm','-M','virt-5.2','-cpu','cortex-a7','-smp','2','-m','48M',
            '-nodefaults','-display','none','-monitor','none','-no-reboot','-serial','file:'+str(serial),'-nic','none',
            '-global','virtio-mmio.force-legacy=true','-object','rng-random,filename=/dev/urandom,id=rng0',
            '-device','virtio-rng-device,rng=rng0','-kernel',str(kernel),'-initrd',str(initrd),'-append',
            'console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init rng_core.default_quality=1000 qdc.sms.pty=1']

def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('base',type=Path);p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--variant',choices=('old','new'),required=True);p.add_argument('--attempt',type=int,required=True)
    args=p.parse_args()
    if args.attempt<1:p.error('attempt must be positive')
    base=args.base.resolve();binary=args.binary.resolve();source=Path(__file__).resolve().parent
    logs=base/'sms-opt-build/pty-arm-logs'/f'{args.variant}-{args.attempt}';logs.mkdir(mode=0o700,parents=True,exist_ok=False)
    command=['docker','run','--rm','--network','none','--platform','linux/amd64','-v',str(source)+':/src:ro',
             '-v',str(binary)+':/input/pty-tests:ro','-v',str(logs)+':/out','-v','qdc507-ike-build-v1:/work',
             'qdc507-ike-builder:local','sh','/src/build-rootfs.sh']
    (logs/'build-command.json').write_text(json.dumps(command,indent=2)+'\n')
    with (logs/'build.log').open('wb') as output:subprocess.run(command,check=True,stdout=output,stderr=subprocess.STDOUT)
    kernel=base/'runtime-build/lab-only-runtime-zImage';initrd=logs/'lab-only-pty-rootfs.cpio.gz'
    cmd=qemu_command(kernel,initrd,logs/'serial.log');(logs/'qemu-command.json').write_text(json.dumps(cmd,indent=2)+'\n')
    digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    result={'status':'failed','variant':args.variant,'device_executed':False,'test_binary_sha256':digest(binary),'kernel_sha256':digest(kernel),
            'initrd_sha256':digest(initrd),'fixture_source_sha256':{name:digest(source/name) for name in ('init.sh','build-rootfs.sh','run_pty.py')},'network':'none','memory_mib':48}
    with (logs/'qemu-stderr.log').open('wb') as error:
        proc=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=error)
        try:
            deadline=time.monotonic()+50
            while time.monotonic()<deadline:
                text=(logs/'serial.log').read_text(errors='replace') if (logs/'serial.log').exists() else ''
                if 'SMS_PTY_COMPLETE' in text or proc.poll() is not None:break
                time.sleep(.1)
            else:raise TimeoutError('ARM PTY guest deadline')
            result.update(validate_log(text,args.variant))
        except Exception as error:
            result['error']=str(error);raise
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
            (logs/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'status':result['status'],'variant':args.variant,'logs':str(logs),'test_binary_sha256':result['test_binary_sha256']}))

if __name__=='__main__':main()
