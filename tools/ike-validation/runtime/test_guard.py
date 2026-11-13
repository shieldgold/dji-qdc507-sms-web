"""Linux CPU tests of the supervisor CLI; no namespaces or TUN are accessed."""
import os
from pathlib import Path
import subprocess
import unittest

GUARD = Path(os.environ.get('QDC_IKE_GUARD', '/out/qdc-ike-guard-native'))


class GuardTests(unittest.TestCase):
    def run_guard(self, *args):
        self.assertTrue(GUARD.exists(), 'native supervisor is not implemented')
        return subprocess.run([str(GUARD), *args], input=b'', capture_output=True, timeout=5)

    def test_invalid_request_is_rejected(self):
        for args in [[], ['idle', '/tmp/x', 'TEST', '10'],
                     ['idle', '/run/qdc-tun-TEST', 'bad;id', '10'],
                     ['idle', '/run/qdc-tun-TEST', 'TEST', '61'],
                     ['resume', '1', '0', '999999']]:
            with self.subTest(args=args):
                self.assertNotEqual(self.run_guard(*args).returncode, 0)

    def test_selftest_does_not_launch_client(self):
        result = self.run_guard('selftest')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'SELFTEST_OK', result.stdout)
        self.assertNotIn(b'CHARON', result.stdout)

    def test_pid_reuse_identity_is_refused(self):
        self.assertNotEqual(self.run_guard('identity', str(os.getpid()), '1', '/bin/busybox').returncode, 0)

    def test_describe_reads_parent_identity(self):
        result = self.run_guard('describe', str(os.getpid()))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(('pid='+str(os.getpid())).encode(), result.stdout)
        self.assertIn(b'start=', result.stdout)


if __name__ == '__main__':
    unittest.main()
