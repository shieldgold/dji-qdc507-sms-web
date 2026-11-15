"""Prevent incomplete or failed ARM evidence from being reported as accepted."""
import unittest

from run_vm import MANUAL_CASES, validate_manual, validate_soak


def manual_evidence():
    return '\n'.join([
        *(f'SMS_MANUAL_VM_CASE {case} PASS' for case in MANUAL_CASES),
        'SMS_MANUAL_VM_PREP_ELAPSED seconds=120.123',
        'SMS_MANUAL_VM_PASS cases=10 original_unchanged=1 install_retained=1 device_executed=0',
    ])


def soak_evidence(case='normal'):
    common = '\n'.join([
        f'SMS_SOAK_VM_PASS case={case}',
        'SMS_SOAK_VM_NETWORK_UNCHANGED',
        'SMS_SOAK_VM_PRODUCTION_PROOF_NOT_FAKED',
        'MANUAL_ROLLBACK_COMPLETE source_retained=1',
    ])
    if case == 'normal':
        return common + '\n' + '\n'.join([
            *(f'SOAK_RECONNECTED index={n} established=1 installed=1 protected_ping=1' for n in range(1, 4)),
            'SOAK_REKEY_COMPLETE inbound_and_outbound_spi_changed=1 protected_ping=1',
            'SOAK_TRAFFIC_COMPLETE real_seconds=120.123 window_seconds=3603.690 ping_pairs=350 http_success=350 reconnects=3 rekeys=1',
            'SOAK_COMPLETE rc=0 children_reaped=1',
        ])
    return common + '\n' + {
        'wrong_baseline': 'SOAK_REFUSED stage=pre_c_baseline_proof',
        'no_go': 'LOGGER_NOT_ARMED',
        'http_failure': 'HTTP_PROBE status=503',
        'signal_abort': 'SOAK_COMPLETE rc=1 children_reaped=1',
    }[case]


class EvidenceTests(unittest.TestCase):
    def test_complete_manual_evidence(self):
        self.assertEqual(len(validate_manual(manual_evidence())['cases']), 10)

    def test_each_manual_case_required(self):
        for case in MANUAL_CASES:
            with self.subTest(case=case), self.assertRaises(ValueError):
                validate_manual(manual_evidence().replace(f'SMS_MANUAL_VM_CASE {case} PASS', ''))

    def test_real_deadline_not_accelerated(self):
        for duration in ('0', '118.999', '128', 'nan'):
            with self.subTest(duration=duration), self.assertRaises(ValueError):
                validate_manual(manual_evidence().replace('seconds=120.123', 'seconds=' + duration))

    def test_failed_guest_not_accepted(self):
        for marker in ('SMS_SWAP_VM_FAILED', 'Kernel panic'):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                validate_manual(manual_evidence() + '\n' + marker)

    def test_scaled_soak_does_not_claim_hardware_hour(self):
        result = validate_soak('normal', soak_evidence())
        self.assertTrue(result['production_hour_not_claimed'])
        self.assertTrue(result['scaled_functional_only'])
        self.assertEqual(result['clock_rate'], 30)

    def test_short_or_incomplete_traffic_rejected(self):
        for before, after in [('real_seconds=120.123', 'real_seconds=119.999'),
                              ('window_seconds=3603.690', 'window_seconds=3599.999'),
                              ('ping_pairs=350', 'ping_pairs=299'),
                              ('http_success=350', 'http_success=349')]:
            with self.subTest(after=after), self.assertRaises(ValueError):
                validate_soak('normal', soak_evidence().replace(before, after))

    def test_distinct_reconnects_and_real_rekey_required(self):
        for before, after in [('index=2 ', 'index=1 '),
                              ('inbound_and_outbound_spi_changed=1', 'inbound_and_outbound_spi_changed=0'),
                              ('SOAK_COMPLETE rc=0', 'SOAK_COMPLETE rc=1')]:
            with self.subTest(after=after), self.assertRaises(ValueError):
                validate_soak('normal', soak_evidence().replace(before, after))

    def test_cleanup_proofs_all_required(self):
        for marker in ('SMS_SOAK_VM_NETWORK_UNCHANGED', 'SMS_SOAK_VM_PRODUCTION_PROOF_NOT_FAKED',
                       'MANUAL_ROLLBACK_COMPLETE source_retained=1'):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                validate_soak('normal', soak_evidence().replace(marker, ''))

    def test_faults_need_expected_stop(self):
        for case in ('wrong_baseline', 'no_go', 'http_failure', 'signal_abort'):
            with self.subTest(case=case):
                self.assertEqual(validate_soak(case, soak_evidence(case))['status'], 'passed')
            with self.subTest(case=case), self.assertRaises(ValueError):
                validate_soak(case, soak_evidence(case).rsplit('\n', 1)[0])

    def test_refused_or_unarmed_must_not_start_vpn(self):
        for case in ('wrong_baseline', 'no_go'):
            with self.subTest(case=case), self.assertRaises(ValueError):
                validate_soak(case, soak_evidence(case) + '\nOWNED label=charon pid=123')

    def test_fault_must_not_be_reported_as_success(self):
        for case in ('wrong_baseline', 'no_go', 'http_failure', 'signal_abort'):
            with self.subTest(case=case), self.assertRaises(ValueError):
                validate_soak(case, soak_evidence(case) + '\nSOAK_COMPLETE rc=0 children_reaped=1')


if __name__ == '__main__':
    unittest.main()
