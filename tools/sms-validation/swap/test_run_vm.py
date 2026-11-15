import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from run_vm import CASES, validate

class RunnerTests(unittest.TestCase):
    def valid_log(self):
        return '\n'.join(f'SMS_SWAP_VM_CASE {case} PASS' for case in CASES)+'\nSMS_SWAP_VM_PASS cases=6 original_bytes_not_overwritten=1 clock_modified=0 device_executed=0\n'

    def test_accepts_complete_six_case_proof(self):
        self.assertEqual(validate(self.valid_log())['status'],'passed')

    def test_missing_or_failed_case_cannot_pass(self):
        for case in CASES:
            with self.assertRaises(ValueError): validate(self.valid_log().replace(f'SMS_SWAP_VM_CASE {case} PASS',''))
        for marker in ('SMS_SWAP_VM_FAILED','Kernel panic','SIGSEGV'):
            with self.assertRaises(ValueError): validate(self.valid_log()+marker)

    def test_replaced_helper_is_rejected_before_starting_qemu(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); out=root/'out'; out.mkdir()
            (out/'lab-only-sms-swap-rootfs.cpio.gz').write_bytes(b'fixture-initrd')
            (out/'qdc-sms-swap-arm').write_bytes(b'replaced-helper')
            kernel=root/'kernel'; kernel.write_bytes(b'fixture-kernel')
            (out/'build.json').write_text(json.dumps({'arm_elf':{'sha256':hashlib.sha256(b'original-helper').hexdigest()},
                'initrd_sha256':hashlib.sha256(b'fixture-initrd').hexdigest()}))
            fake=root/'bin'; fake.mkdir(); marker=root/'qemu-started'
            qemu=fake/'qemu-system-arm'
            qemu.write_text('#!'+sys.executable+'\nfrom pathlib import Path\nPath('+repr(str(marker))+').write_text("started")\n')
            qemu.chmod(0o700)
            env=dict(os.environ,PATH=str(fake)+os.pathsep+os.environ.get('PATH',''))
            result=subprocess.run([sys.executable,str(Path(__file__).with_name('run_vm.py')),str(out),'--kernel',str(kernel)],env=env,capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(marker.exists(),'runner launched QEMU for helper that differs from sealed initrd build')

if __name__=='__main__': unittest.main()
