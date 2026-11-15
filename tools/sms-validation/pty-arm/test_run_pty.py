import unittest
from run_pty import validate_log, qemu_command

TESTS=('TestOpenModemCloseReleasesBlockedPTY','TestModemSetupFailureReleasesPTYDescriptor')
class PTYEvidenceTest(unittest.TestCase):
    def test_old_requires_the_two_actual_blocked_close_failures(self):
        text='SMS_PTY_KERNEL release=3.18.44\n'+\
             '    closing modem blocked behind its kernel PTY read\n--- FAIL: '+TESTS[0]+' (1.00s)\n'+\
             '    failed setup blocked while closing its PTY reader\n--- FAIL: '+TESTS[1]+' (2.00s)\n'+\
             'SMS_PTY_EXIT code=1\nSMS_PTY_COMPLETE\n'
        self.assertEqual(validate_log(text,'old').get('status'),'expected_red')
        with self.assertRaises(ValueError):validate_log(text.replace('closing modem blocked behind its kernel PTY read','unrelated failure'),'old')
    def test_new_requires_both_successes(self):
        text='SMS_PTY_KERNEL release=3.18.44\n'+''.join('--- PASS: '+n+' (0.01s)\n' for n in TESTS)+'PASS\nSMS_PTY_EXIT code=0\nSMS_PTY_COMPLETE\n'
        self.assertEqual(validate_log(text,'new').get('status'),'passed')
        with self.assertRaises(ValueError):validate_log(text.replace(TESTS[1],'other'),'new')
    def test_no_network_or_usb(self):
        cmd=qemu_command('/kernel','/initrd','/serial')
        self.assertIn('-nic',cmd);self.assertIn('none',cmd)
        self.assertNotIn('-netdev',cmd);self.assertNotIn('-drive',cmd);self.assertNotIn('-usb',cmd)

if __name__=='__main__':unittest.main()
