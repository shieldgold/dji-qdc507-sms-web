import unittest
import run

GOOD = '\n'.join(('CLOCK_FAILURE_KERNEL release=3.18.44',
    'CLOCK_FAILURE_SUPERVISOR identity=exact_busybox_argv',
    'CLOCK_FAILURE_CAPS uid=0 effective=0 permitted=0 inheritable=0 bounding=0 cap_sys_time=25',
    'RESUME_GUARD_READY restore_clock=1',
    'RECOVERY_REQUIRED clock_restore: Operation not permitted',
    'CLOCK_FAILURE_OBSERVED case=failure rc=1 supervisor_state=T resume_sent=0 delegated=0',
    'CLOCK_FAILURE_CLEANUP owned_supervisor_group_removed=1', 'CLOCK_FAILURE_PASS'))


class ClockFailureTests(unittest.TestCase):
    def test_complete_failure_proof_is_required(self):
        run.validate(GOOD)
        for line in GOOD.splitlines():
            with self.subTest(line=line):
                with self.assertRaises(ValueError): run.validate(GOOD.replace(line, ''))

    def test_resume_or_restoration_claim_is_refused(self):
        for line in ('RESUME_SENT', 'RESUME_DELEGATED', 'CLOCK_RESTORED', 'CLOCK_FAILURE_FAILED', 'Kernel panic'):
            with self.assertRaises(ValueError): run.validate(GOOD+'\n'+line)

    def test_delegation_requires_clock_order_and_stopped_supervisor(self):
        good = GOOD.replace('CLOCK_FAILURE_CAPS uid=0 effective=0 permitted=0 inheritable=0 bounding=0 cap_sys_time=25',
                            'CLOCK_RESTORED epoch=315532802 before_supervisor_cont=1').replace(
                            'RECOVERY_REQUIRED clock_restore: Operation not permitted',
                            'RESUME_DELEGATED verify_supervisor_before_cont=1').replace(
                            'case=failure rc=1 supervisor_state=T resume_sent=0 delegated=0',
                            'case=delegate-success rc=0 supervisor_state=T resume_sent=0 delegated=1 epoch=315532802')
        run.validate(good, 'delegate-success')
        for wrong in (good.replace('supervisor_state=T', 'supervisor_state=R'),
                      good.replace('epoch=315532802', 'epoch=1794700800'),
                      good+'\nRESUME_SENT', good.replace('RESUME_DELEGATED', 'missing-proof')):
            with self.assertRaises(ValueError): run.validate(wrong, 'delegate-success')

    def test_delegate_failure_cannot_claim_delegation(self):
        good = GOOD.replace('case=failure', 'case=delegate-failure')
        run.validate(good, 'delegate-failure')
        with self.assertRaises(ValueError): run.validate(good+'\nRESUME_DELEGATED', 'delegate-failure')

    def test_private_vm_has_no_network_or_hardware(self):
        cmd = run.command('/kernel', '/initrd', '/serial')
        self.assertEqual(cmd[cmd.index('-nic')+1], 'none')
        self.assertIn('qdc.clockfailure.lab=1', cmd[-1])
        self.assertNotIn('-device', cmd)
        self.assertNotIn('-netdev', cmd)


if __name__ == '__main__': unittest.main()
