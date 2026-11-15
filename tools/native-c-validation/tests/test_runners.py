import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from run_vm import validate_log,qemu_command
from run_coexist import validate,command
from test_preflight import START,READY

def sms_log():
    lines=['SMS_LAB_KERNEL release=3.18.44','SMS_LAB_PTY_OK','SMS_LAB_FUNCTIONAL_PASS','SMS_LAB_PASS scenario=ready',
           'SMS_LAB_FAKE commands=30 polls=3 submitted=1 cancelled=0 unlocks=0']
    lines+=['SMS_LAB_SAMPLE phase='+phase+' rss_kib=1000 pss_kib=996 maps=20 mem_available_kib=30000' for phase in ('startup','poll','http','pressure','released')]
    return '\n'.join(lines)
def combo_log():
    return '\n'.join(['SMS_COMBO_REAL_SMS_STARTED','SMS_COMBO_FUNCTIONAL_PASS','SMS_COMBO_CLOCK_ORDER_OK',
        'SMS_COMBO_NETWORK_UNCHANGED','SMS_COMBO_VPN_CLEANED','COEXIST_POST_SMS_OK same_identity=1 http_status=200',
        'COEXIST_COMPLETE rc=0 children_reaped=1','COEXIST_TRAFFIC_COMPLETE seconds=30.001 ping_pairs=10 http_success=10',
        'SMS_COMBO_FAKE commands=100 polls=12 submitted=1 cancelled=0 unlocks=0','SMS_COMBO_PASS']+
        ['COEXIST_PAIR index=%d ping=1 http_status=200'%i for i in range(1,11)])
class RunnerPreflightTest(unittest.TestCase):
    def test_sms_requires_entropy_before_functional_evidence(self):
        with self.assertRaises(ValueError):validate_log(sms_log(),'ready')
        self.assertEqual(validate_log(START+'\n'+READY+'\n'+sms_log(),'ready')['status'],'passed')
    def test_coexist_requires_entropy_before_runtime_evidence(self):
        with self.assertRaises(ValueError):validate(combo_log())
        prefix=START.replace('gate=sms','gate=coexist')+'\n'+READY+'\n'
        self.assertEqual(validate(prefix+combo_log())['status'],'passed')
    def test_preflight_must_precede_fixture(self):
        with self.assertRaises(ValueError):validate_log(sms_log()+'\n'+START+'\n'+READY,'ready')
        prefix=START.replace('gate=sms','gate=coexist')+'\n'+READY
        with self.assertRaises(ValueError):validate(combo_log()+'\n'+prefix)
    def test_go_baseline_still_requires_gc_metadata(self):
        text=START+'\n'+READY+'\n'+sms_log()
        with self.assertRaises(ValueError):validate_log(text,'ready','go')
        result=validate_log(text+'\ngc 1 @0.1s: 2->1->1 MB, 4 MB goal','ready','go')
        self.assertTrue(result['requires_go_gc_metadata'])
        self.assertEqual(result['go_gc'][0]['heap_live_mib'],1)
    def test_sms_remains_network_free(self):
        cmd=qemu_command('/kernel','/initrd','/serial','ready')
        self.assertIn('-nic',cmd);self.assertIn('none',cmd)
        self.assertNotIn('-usb',cmd);self.assertNotIn('-drive',cmd)
        self.assertIn('maxcpus=1',cmd[-1])
    def test_combo_remains_private_loopback_pair(self):
        cmd=command('/kernel','/initrd','/serial','client',34567)
        self.assertIn('socket,id=private,connect=127.0.0.1:34567',cmd)
        self.assertNotIn('user',cmd);self.assertNotIn('-usb',cmd);self.assertNotIn('-drive',cmd)
if __name__=='__main__':unittest.main()
