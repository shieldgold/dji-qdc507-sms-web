"""CPU-only boundary tests. No USB, TUN or namespace operations are requested."""
import os
from pathlib import Path
import subprocess
import unittest

GUARD = Path(os.environ.get('QDC_IKE_GUARD', '/out/qdc-ike-guard-native'))


class CoexistGuardTests(unittest.TestCase):
    def run_guard(self, *args):
        return subprocess.run([str(GUARD), *args], input=b'', capture_output=True, timeout=5)

    def test_valid_coexist_syntax_reaches_trusted_root_refusal(self):
        result = self.run_guard('coexist', '/tmp/untrusted', 'TEST_COEX', '30', '10.99.0.1', '10.88.0.2')
        self.assertEqual(result.returncode, 65, result.stderr)

    def test_coexist_window_and_targets_are_fixed_before_device_access(self):
        for seconds, target, vip in [('29', '10.99.0.1', '10.88.0.2'),
                                     ('60', '10.99.0.1', '10.88.0.2'),
                                     ('30', '127.0.0.1', '10.88.0.2'),
                                     ('30', '10.99.0.1', '10.99.0.1')]:
            self.assertEqual(self.run_guard('coexist', '/tmp/untrusted', 'TEST_COEX', seconds, target, vip).returncode, 64)

    def test_http_command_rejects_public_and_nonlocal_addresses(self):
        for address in ('8.8.8.8', '10.231.249.253', '127.0.0.1;id', '::1'):
            result = self.run_guard('http-once', address)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'HTTP_PROBE', result.stdout)


if __name__ == '__main__':
    unittest.main()
