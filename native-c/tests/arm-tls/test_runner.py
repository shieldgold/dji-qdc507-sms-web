import importlib.util
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location('arm_tls_runner', Path(__file__).with_name('run.py'))
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


def passing_log():
    lines = [
        'QDC_TLS_KERNEL release=3.18.44 machine=armv7l epoch=1794729600 dual_gate=1 network=loopback-only',
        'QDC_ENTROPY_PREFLIGHT gate=sms timeout_ms=90000 admission_kib=8192 absolute_floor_kib=6144 max_drop_kib=3072',
        'QDC_ENTROPY_READY elapsed_ms=60100 getrandom_return=32 errno=0 attempts=602 min_available_kib=30000 reason=ready',
        'QDC_TLS_RUNTIME_BEGIN admission_kib=8192 absolute_floor_kib=6144 max_drop_kib=3072 baseline_available_kib=30000 getrandom_return=32',
        'tls unit: PASS (HTTP Date validation, DNS ID/question/truncation/pointer-loop rejection)',
    ]
    for name in RUNNER.CASES:
        negative = name in ('wrongname', 'wrongca')
        lines.append('QDC_TLS_CASE_BEGIN case=' + name)
        if negative:
            lines.append('QDC_TLS_PEER_CERTIFICATE_REJECT version=TLSv1.2 fatal_alert=1 description=48 tls_code=-30592 h2_requests=0')
        else:
            version = 'TLSv1.2' if name == 'tls12' else 'TLSv1.3'
            lines.extend([
                f'QDC_TLS_PEER_NEGOTIATED version={version} alpn=h2',
                'QDC_TLS_PEER_REQUEST_COMPLETE method=POST headers=6 body_bytes=2 request_end_stream=1',
                'QDC_TLS_PEER_RESPONSE_COMPLETE status=200 body_bytes=12 response_end_stream=1',
                'QDC_TLS_PEER_CLIENT_CLOSED closed=1 tls_code=0',
                'QDC_TLS_PEER_PASS case=' + name,
            ])
        lines.append('QDC_TLS_CLIENT case=' + name)
        lines.append('TLS rejected: 苹果推送 TLS 验证失败或超时' if negative else
                     'status=200 h2=1 date=Sun, 15 Nov 2026 08:00:00 GMT body={"arm":true}')
        lines.append(f'QDC_TLS_SAMPLE case={name} peer_rss_kib=900 peer_pss_kib=800 client_rss_kib=900 client_pss_kib=800 interval_ms=20')
        lines.append(f'QDC_TLS_CASE_PASS case={name} outcome={"rejected" if negative else "accepted"} client_exit={int(negative)} peer_exit=0 elapsed_ms=1000 min_available_kib=28000')
    lines.extend(['QDC_TLS_CLEANUP children=0 sockets=0 files=0',
                  'QDC_TLS_RUNTIME_PASS cases=4 baseline_available_kib=30000 min_available_kib=28000'])
    return '\n'.join(lines) + '\n'


class EvidenceTests(unittest.TestCase):
    def test_complete_matrix(self):
        result = RUNNER.validate_log(passing_log())
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(len(result['cases']), 4)
        self.assertEqual(result['cases']['tls12']['version'], 'TLSv1.2')
        self.assertFalse(result['device_executed'])

    def test_missing_or_forged_evidence(self):
        log = passing_log()
        mutations = [
            log.replace('QDC_ENTROPY_READY ', 'QDC_ENTROPY_PENDING '),
            log.replace('getrandom_return=32', 'getrandom_return=-1'),
            log.replace('version=TLSv1.3', 'version=TLSv1.2'),
            log.replace('method=POST', 'method=GET'),
            log.replace('request_end_stream=1', 'request_end_stream=0'),
            log.replace('response_end_stream=1', 'response_end_stream=0'),
            log.replace('body={"arm":true}', 'body={}'),
            log.replace('fatal_alert=1', 'fatal_alert=0'),
            log.replace('description=48', 'description=20'),
            log.replace('h2_requests=0', 'h2_requests=1'),
            log.replace('outcome=rejected client_exit=1', 'outcome=rejected client_exit=0'),
            log.replace('elapsed_ms=1000', 'elapsed_ms=13000'),
            log.replace('min_available_kib=28000', 'min_available_kib=26000'),
            log.replace('min_available_kib=28000', 'min_available_kib=6000'),
            log.replace('children=0', 'children=1'),
            log + 'QDC_TLS_FAILED reason=late-failure\n',
            log + 'Kernel panic - not syncing\n',
            log + 'x' * 1048576,
        ]
        for candidate in mutations:
            with self.subTest(candidate=candidate[-100:]):
                with self.assertRaises(ValueError):
                    RUNNER.validate_log(candidate)

    def test_preflight_must_precede_runtime(self):
        log = passing_log().splitlines()
        log[2], log[3] = log[3], log[2]
        with self.assertRaises(ValueError):
            RUNNER.validate_log('\n'.join(log))

    def test_serial_boot_nul_before_kernel_marker(self):
        log = 'GIC CPU mask not found - kernel will fail to boot.\n\0' + passing_log()
        self.assertEqual(RUNNER.validate_log(log)['status'], 'passed')

    def test_duplicate_case_rejected(self):
        log = passing_log()
        duplicate = log[log.index('QDC_TLS_CASE_BEGIN case=tls12'):log.index('QDC_TLS_CASE_BEGIN case=tls13')]
        with self.assertRaises(ValueError):
            RUNNER.validate_log(log + duplicate)

    def test_qemu_is_sealed_and_single_cpu(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            command = RUNNER.qemu_command(p/'kernel', p/'initrd', p/'serial')
        self.assertEqual(command[command.index('-m') + 1], '48M')
        self.assertEqual(command[command.index('-smp') + 1], '1')
        self.assertEqual(command[command.index('-nic') + 1], 'none')
        self.assertIn('maxcpus=1', command[-1])
        self.assertIn('qdc.sms.lab=1 qdc.tls.lab=1', command[-1])
        self.assertNotIn('-netdev', command)
        self.assertNotIn('-drive', command)


if __name__ == '__main__':
    unittest.main()
