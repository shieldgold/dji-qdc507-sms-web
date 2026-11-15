"""Computer-only ARM proc/mount rollback acceptance; no USB or network devices."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

CASES=('early_clock_handoff','running_new_to_original','clock_proof_refusal','symlink_refusal','clock_window_target_refusal','unknown_overlay_retained')

def validate(text):
    for case in CASES:
        if f'SMS_SWAP_VM_CASE {case} PASS' not in text:
            raise ValueError('missing VM case: '+case)
    if 'SMS_SWAP_VM_PASS cases=6 original_bytes_not_overwritten=1 clock_modified=0 device_executed=0' not in text:
        raise ValueError('missing final VM proof')
    if any(x in text for x in ('SMS_SWAP_VM_FAILED','Kernel panic','SIGSEGV')):
        raise ValueError('VM failed')
    return {'status':'passed','cases':list(CASES),'device_executed':False,'clock_modified':False}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=Path)
    p.add_argument('--kernel',type=Path,required=True)
    a=p.parse_args(); output=a.output.resolve(); kernel=a.kernel.resolve()
    initrd=output/'lab-only-sms-swap-rootfs.cpio.gz'
    serial=output/'vm.log'; error=output/'vm-qemu.log'
    if not kernel.is_file() or not initrd.is_file(): p.error('built kernel and swap initrd required')
    sealed=json.loads((output/'build.json').read_text())
    actual_helper=hashlib.sha256((output/'qdc-sms-swap-arm').read_bytes()).hexdigest()
    actual_initrd=hashlib.sha256(initrd.read_bytes()).hexdigest()
    if actual_helper!=sealed['arm_elf']['sha256'] or actual_initrd!=sealed['initrd_sha256']:
        p.error('artifact changed after sealed helper/initrd build; rebuild before running QEMU')
    if serial.exists(): p.error('refuse to overwrite prior VM evidence; use a fresh build output')
    cmd=['qemu-system-arm','-M','virt-5.2','-cpu','cortex-a7','-smp','1','-m','48M',
         '-nodefaults','-display','none','-monitor','none','-no-reboot','-serial','file:'+str(serial),
         '-nic','none','-kernel',str(kernel),'-initrd',str(initrd),'-append',
         'console=ttyAMA0 loglevel=3 panic=1 rdinit=/init qdc.sms.swap.lab=1']
    result={'status':'failed','device_executed':False,'network':'none; no USB devices',
            'artifact_sha256':{x:hashlib.sha256(y.read_bytes()).hexdigest() for x,y in
                [('kernel',kernel),('initrd',initrd),('helper',output/'qdc-sms-swap-arm')]}}
    (output/'vm-command.json').write_text(json.dumps(cmd,indent=2)+'\n')
    proc=None
    try:
        with error.open('wb') as err:
            proc=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=err)
            until=time.monotonic()+60
            while time.monotonic()<until:
                text=serial.read_text(errors='replace') if serial.exists() else ''
                if 'SMS_SWAP_VM_PASS' in text:
                    result.update(validate(text)); break
                if proc.poll() is not None or 'SMS_SWAP_VM_FAILED' in text or 'Kernel panic' in text:
                    raise RuntimeError('ARM swap guest failed')
                time.sleep(.2)
            else: raise TimeoutError('ARM swap guest deadline')
    except Exception as exc:
        result['error']=str(exc); raise
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        (output/'vm-result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))

if __name__=='__main__': main()
