import unittest
from run_vm import validate_log, qemu_command

class EvidenceTest(unittest.TestCase):
    def test_requires_real_runtime_and_all_phases(self):
        good = '\n'.join([
            'SMS_LAB_KERNEL release=3.18.44',
            'SMS_LAB_PTY_OK', 'SMS_LAB_FUNCTIONAL_PASS',
            'SMS_LAB_SAMPLE phase=startup rss_kib=2000 pss_kib=1900 anonymous_kib=1000 file_rss_kib=1000 maps=20 mem_available_kib=16000',
            'SMS_LAB_SAMPLE phase=poll rss_kib=2100 pss_kib=2000 anonymous_kib=1100 file_rss_kib=1000 maps=21 mem_available_kib=15900',
            'SMS_LAB_SAMPLE phase=http rss_kib=2400 pss_kib=2300 anonymous_kib=1400 file_rss_kib=1000 maps=22 mem_available_kib=15600',
            'SMS_LAB_SAMPLE phase=pressure rss_kib=2400 pss_kib=2300 anonymous_kib=1400 file_rss_kib=1000 maps=22 mem_available_kib=11500',
            'SMS_LAB_SAMPLE phase=released rss_kib=2300 pss_kib=2200 anonymous_kib=1300 file_rss_kib=1000 maps=22 mem_available_kib=15500',
            'gc 1 @0.010s 0%: 0.1+0.2+0.1 ms clock, 0.1+0/0/0+0.1 ms cpu, 2->2->1 MB, 3 MB goal, 0 MB stacks, 0 MB globals, 1 P',
            'pacer: 25% CPU (25 exp.) for 123+456+789 B work (1200 B exp.) in 1000000 B -> 1200000 B (∆goal -100, cons/mark +1.000000e+000)',
            'SMS_LAB_FAKE commands=100 polls=5 submitted=1 cancelled=0 unlocks=0',
            'SMS_LAB_PASS scenario=ready'])
        result = validate_log(good, 'ready')
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(result['samples']['startup']['rss_kib'], 2000)
        self.assertEqual(result['go_gc'][-1]['heap_live_mib'], 1)
        self.assertEqual(result.get('go_pacer'), [{'heap_at_trigger_bytes':1000000,'heap_at_mark_end_bytes':1200000}])
        for bad in [good.replace('SMS_LAB_PTY_OK',''),
                    good.replace('phase=pressure','phase=other'),
                    good+'\nSIGSEGV: segmentation violation',
                    good.replace('submitted=1','submitted=0')]:
            with self.assertRaises(ValueError): validate_log(bad, 'ready')

    def test_emulator_has_no_network_or_device_mounts(self):
        args = qemu_command('/tmp/kernel','/tmp/initrd','/tmp/serial','ready')
        self.assertIn('none',args)
        self.assertIn('-nic',args)
        self.assertNotIn('-netdev',args)
        self.assertNotIn('-usb',args)
        self.assertNotIn('-drive',args)
        self.assertIn('48M',args)

if __name__ == '__main__': unittest.main()
