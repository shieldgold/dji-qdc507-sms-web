import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from preflight import validate_preflight

START='QDC_ENTROPY_PREFLIGHT gate=sms timeout_ms=90000 admission_kib=8192 absolute_floor_kib=6144 max_drop_kib=3072'
READY='QDC_ENTROPY_READY elapsed_ms=60100 getrandom_return=32 errno=0 attempts=602 min_available_kib=32000 reason=ready'
class PreflightEvidenceTest(unittest.TestCase):
    def test_requires_actual_getrandom_success(self):
        self.assertIsNotNone(validate_preflight(START+'\n'+READY,'sms'))
    def test_accepts_serial_boot_nul_prefix(self):
        self.assertIsNotNone(validate_preflight('\x00'+START+'\n'+READY,'sms'))
    def test_rejects_missing_or_wrong_gate(self):
        for text in ('',READY,START,START.replace('gate=sms','gate=coexist')+'\n'+READY):
            with self.assertRaises(ValueError):validate_preflight(text,'sms')
    def test_rejects_weakened_guards_or_timeout(self):
        for text in ((START+'\n'+READY).replace('max_drop_kib=3072','max_drop_kib=4096'),
                     START+'\n'+READY.replace('elapsed_ms=60100','elapsed_ms=90001'),
                     START+'\n'+READY.replace('min_available_kib=32000','min_available_kib=6143')):
            with self.assertRaises(ValueError):validate_preflight(text,'sms')
    def test_rejects_short_or_failed_random(self):
        for text in (START+'\n'+READY.replace('getrandom_return=32','getrandom_return=31'),
                     START+'\n'+READY.replace('errno=0','errno=11'),
                     START+'\n'+READY+'\nQDC_ENTROPY_FAILED reason=timeout'):
            with self.assertRaises(ValueError):validate_preflight(text,'sms')
    def test_rejects_reordered_or_repeated_evidence(self):
        for text in (READY+'\n'+START,START+'\n'+READY+'\n'+READY):
            with self.assertRaises(ValueError):validate_preflight(text,'sms')
    def test_accepts_coexist_gate(self):
        result=validate_preflight(START.replace('gate=sms','gate=coexist')+'\n'+READY,'coexist')
        self.assertIsNotNone(result)
if __name__=='__main__':unittest.main()
