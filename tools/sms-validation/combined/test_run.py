import unittest
from run import validate, command

class CombinedEvidenceTest(unittest.TestCase):
    def test_requires_full_sms_and_guarded_thirty_seconds(self):
        good='\n'.join(['SMS_COMBO_REAL_SMS_STARTED','SMS_COMBO_FUNCTIONAL_PASS',
            'SMS_COMBO_CLOCK_ORDER_OK','SMS_COMBO_NETWORK_UNCHANGED','SMS_COMBO_VPN_CLEANED',
            'COEXIST_POST_SMS_OK same_identity=1 http_status=200',
            'COEXIST_COMPLETE rc=0 children_reaped=1',
            'COEXIST_TRAFFIC_COMPLETE seconds=30.001 ping_pairs=10 http_success=10',
            'SMS_COMBO_FAKE commands=100 polls=12 submitted=1 cancelled=0 unlocks=0',
            'SMS_COMBO_PASS']+['COEXIST_PAIR index=%d ping=1 http_status=200'%i for i in range(1,11)])
        self.assertEqual(validate(good).get('status'),'passed')
        for bad in (good.replace('SMS_COMBO_FUNCTIONAL_PASS',''),good.replace('seconds=30.001','seconds=29.999'),
                    good.replace('index=10 ','index=9 '),good+'\nSTOP memory available_kib=20000'):
            with self.assertRaises(ValueError):validate(bad)
    def test_private_link_has_no_wan_or_usb(self):
        cmd=command('/kernel','/initrd','/serial','client',34567)
        self.assertIn('socket,id=private,connect=127.0.0.1:34567',cmd)
        self.assertNotIn('user',cmd);self.assertNotIn('-usb',cmd);self.assertNotIn('-drive',cmd)

if __name__=='__main__':unittest.main()
