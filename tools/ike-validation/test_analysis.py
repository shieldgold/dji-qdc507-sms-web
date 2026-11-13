import unittest
from analyze_lab import analyze

def complete_log():
    s = 'MEASUREMENT_METHOD smaps_read_to_eof=1\nLAB_GUEST_BEGIN role=client threads=8 auth=pubkey\nESTABLISHED, IKEv2\nINSTALLED, TUNNEL\n100 packets transmitted, 100 packets received, 0% packet loss\n'
    for label in ['IDLE', 'NEGOTIATION', 'TRAFFIC', 'REKEY', 'ESTABLISHED']:
        s += f'MEMORY label={label} nominal_ms=0 rss_kib=1234 hwm_kib=1800 vsize_kib=99999 threads=10 available_kib=8000\n'
        s += f'MEMORY_SUMMARY label={label} sampled_max_rss_kib=1500 lifetime_hwm_kib=1800 min_available_kib=7900\n'
    return s+'CLI_MEMORY label=initiate maxrss_kib=700 rc=0\nLAB_CLIENT_COMPLETE auth=pubkey threads=8\n'

class AnalysisTests(unittest.TestCase):
    def test_rss_not_virtual_size_and_conservative_cli_overlap(self):
        r = analyze(complete_log())
        self.assertEqual(r['daemon_peak_rss_kib'], 1800)
        self.assertEqual(r['conservative_daemon_plus_cli_kib'], 2500)
    def test_incomplete_or_failed_lab_is_not_a_measurement(self):
        for s in [complete_log().replace('LAB_CLIENT_COMPLETE', 'INTERRUPTED'), complete_log()+'Kernel panic\n', complete_log().replace('rc=0', 'rc=1')]:
            with self.assertRaises(ValueError): analyze(s)
    def test_missing_phase_is_rejected(self):
        with self.assertRaises(ValueError): analyze(complete_log().replace('MEMORY_SUMMARY label=REKEY', 'PARTIAL label=REKEY'))
    def test_old_partial_smaps_sampler_is_rejected(self):
        with self.assertRaises(ValueError): analyze(complete_log().replace('MEASUREMENT_METHOD smaps_read_to_eof=1', 'OLD_SAMPLER'))

if __name__ == '__main__': unittest.main()
