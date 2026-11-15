"""Run the complete ARM smsweb in a disposable, network-free Linux 3.18.44 VM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

PHASES = ('startup', 'poll', 'http', 'pressure', 'released')

def validate_log(text, scenario):
    for marker in ('SMS_LAB_KERNEL release=3.18.44', 'SMS_LAB_PTY_OK',
                   'SMS_LAB_FUNCTIONAL_PASS', 'SMS_LAB_PASS scenario='+scenario):
        if marker not in text: raise ValueError('missing evidence: '+marker)
    for marker in ('SMS_LAB_FAILED', 'Kernel panic', 'SIGSEGV', 'fatal error:',
                   'Unable to handle kernel', 'Out of memory:'):
        if marker in text: raise ValueError('runtime failure: '+marker)
    samples = {}
    for match in re.finditer(r'^SMS_LAB_SAMPLE phase=(\w+) (.+)$', text, re.M):
        values = {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',match[2])}
        if not all(values.get(k,0)>0 for k in ('rss_kib','pss_kib','maps','mem_available_kib')):
            raise ValueError('incomplete process sample')
        samples[match[1]] = values
    if any(phase not in samples for phase in PHASES): raise ValueError('missing phase sample')
    fake = re.search(r'SMS_LAB_FAKE commands=(\d+) polls=(\d+) submitted=(\d+) cancelled=(\d+) unlocks=(\d+)', text)
    if not fake or int(fake[2]) < 3 or int(fake[3]) != 1:
        raise ValueError('synthetic modem polling/submission not demonstrated')
    if scenario == 'pin' and int(fake[5]) != 1: raise ValueError('PIN unlock not demonstrated')
    gc = [{'heap_before_mib':int(a), 'heap_after_mib':int(b), 'heap_live_mib':int(c), 'heap_goal_mib':int(d)}
          for a,b,c,d in re.findall(r'(\d+)->(\d+)->(\d+) MB, (\d+) MB goal', text)]
    pacer = [{'heap_at_trigger_bytes':int(a), 'heap_at_mark_end_bytes':int(b)}
             for a,b in re.findall(r'pacer: [^\n]+ in (\d+) B -> (\d+) B', text)]
    if not gc: raise ValueError('no Go heap GC metadata')
    return {'status':'passed', 'scenario':scenario, 'device_executed':False,
            'samples':samples, 'go_gc':gc, 'go_pacer':pacer, 'go_heap_is_not_rss':True,
            'fake_modem':dict(zip(('commands','polls','submitted','cancelled','unlocks'),map(int,fake.groups())))}

def qemu_command(kernel, initrd, serial, scenario):
    if scenario not in ('ready','pin'): raise ValueError('invalid scenario')
    return ['qemu-system-arm','-M','virt-5.2','-cpu','cortex-a7','-smp','2','-m','48M',
            '-nodefaults','-display','none','-monitor','none','-no-reboot',
            '-serial','file:'+str(serial),'-nic','none',
            '-global','virtio-mmio.force-legacy=true',
            '-object','rng-random,filename=/dev/urandom,id=rng0',
            '-device','virtio-rng-device,rng=rng0','-kernel',str(kernel),'-initrd',str(initrd),
            '-append','console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init '+
            'rng_core.default_quality=1000 qdc.sms.lab=1 qdc.sms.case='+scenario]

def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('base',type=Path)
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--label',required=True)
    p.add_argument('--scenario',choices=('ready','pin'),default='ready')
    p.add_argument('--attempt',type=int,required=True)
    args=p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}',args.label) or args.attempt<1: p.error('invalid label/attempt')
    base=args.base.resolve(); out=base/'sms-opt-build'; binary=args.binary.resolve()
    if not binary.is_file(): p.error('missing binary')
    logs=out/'logs'/f'{args.label}-{args.scenario}-{args.attempt}'
    logs.mkdir(mode=0o700,parents=True,exist_ok=False)
    source=Path(__file__).resolve().parent
    # A unique /work/sms-* root is created and removed only by build_guest.sh.
    build=['docker','run','--rm','--network','none','--platform','linux/amd64',
           '-v',str(source)+':/src:ro','-v',str(binary)+':/input/smsweb:ro',
           '-v',str(logs)+':/out','-v','qdc507-ike-build-v1:/work',
           'qdc507-ike-builder:local','sh','/src/build_guest.sh']
    (logs/'build-command.json').write_text(json.dumps(build,indent=2)+'\n')
    with (logs/'build.log').open('wb') as build_log:
        subprocess.run(build,check=True,stdout=build_log,stderr=subprocess.STDOUT)
    kernel=base/'runtime-build/lab-only-runtime-zImage'
    initrd=logs/'lab-only-sms-rootfs.cpio.gz'
    command=qemu_command(kernel,initrd,logs/'serial.log',args.scenario)
    (logs/'qemu-command.json').write_text(json.dumps(command,indent=2)+'\n')
    result={'status':'failed','device_executed':False,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
            'kernel_sha256':hashlib.sha256(kernel.read_bytes()).hexdigest(),
            'initrd_sha256':hashlib.sha256(initrd.read_bytes()).hexdigest(),
            'fixture_sha256':hashlib.sha256((logs/'lab-only-sms-fixture').read_bytes()).hexdigest(),
            'fixture_source_sha256':{name:hashlib.sha256((source/name).read_bytes()).hexdigest()
                                     for name in ('fake_modem.h','fake_modem.c','http_metadata.h','http_metadata.c','fixture.c','init.sh','build_guest.sh','run_vm.py')},
            'network':'none; guest loopback only','memory_mib':48,'gomaxprocs':1,
            'gogc':20,'soft_go_memory_limit_mib':12,'poll_seconds':3}
    with (logs/'qemu-stderr.log').open('wb') as error:
        proc=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=error)
        try:
            deadline=time.monotonic()+180
            while time.monotonic()<deadline:
                text=(logs/'serial.log').read_text(errors='replace') if (logs/'serial.log').exists() else ''
                if 'SMS_LAB_PASS scenario='+args.scenario in text or proc.poll() is not None: break
                if 'SMS_LAB_FAILED' in text or 'Kernel panic' in text: break
                time.sleep(.25)
            else: raise TimeoutError('guest deadline')
            result.update(validate_log(text,args.scenario))
        except Exception as error:
            result['error']=str(error)
            raise
        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
            (logs/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'status':result['status'],'logs':str(logs),'binary_sha256':result['binary_sha256'],
                      'samples':result['samples'],'fake_modem':result['fake_modem']}))

if __name__ == '__main__': main()
