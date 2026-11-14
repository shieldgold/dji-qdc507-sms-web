"""Offline preparation and binding tests using synthetic artifacts/identities."""
import copy
import hashlib
from datetime import datetime, timezone, timedelta
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
import test_prepare

HERE = Path(__file__).resolve().parent


class CoexistBundleTests(unittest.TestCase):
    def load(self):
        self.assertTrue((HERE/'prepare_coexist.py').exists(), 'coexist preparer is not implemented')
        spec = importlib.util.spec_from_file_location('prepare_coexist', HERE/'prepare_coexist.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        return module

    def snapshot(self, run='LAB01'):
        return {'schema': 1, 'observed_at': datetime.now(timezone.utc).isoformat(),
                'boot_id': '01234567-89ab-cdef-0123-456789abcdef', 'http_address': '192.168.225.1',
                'original_sms_gone': True,
                'supervisor': {'pid': 431, 'start': 813, 'exe': '/bin/busybox', 'state': 'T',
                    'argv': ['/bin/busybox', '/bin/sh', '/data/smsweb/start.sh', 'supervise']},
                'original_sms': {'pid': 2000, 'start': 12345, 'exe': '/data/smsweb/smsweb'},
                'guardian': {'pid': 2001, 'start': 12346, 'exe': f'/run/qdc-tun-{run}/qdc-ike-guard',
                    'argv': [f'/run/qdc-tun-{run}/qdc-ike-guard', 'resume-clock', '431', '813', '300']}}

    def bundle(self, root, *, admission=12288, approval=None):
        from prepare import build_bundle
        sources, pins = test_prepare.BundleTests().fixture(root)
        connection = {'mode': 'coexist', 'target': '10.99.0.1', 'vip': '10.88.0.2',
            'transport': 'direct', 'peer_address': '10.77.0.1',
            'files': {n: b'lab-only' for n in ('client.key', 'client.pem', 'ca.pem', 'swanctl.conf')}}
        out = root/'bundle'
        kwargs = {'low_copy': True, 'client_admission_kib': admission}
        if approval is not None: kwargs['coexist_admission_approved_for'] = approval
        manifest = build_bundle(sources, pins, out, 'LAB01', connection, **kwargs)
        return out, manifest

    def test_lower_coexist_admission_requires_matching_run_approval(self):
        self.load()
        for admission, approval in ((11776,None), (11776,'OTHER_RUN'), (12288,'LAB01'), (11776.0,'LAB01')):
            with tempfile.TemporaryDirectory() as td, self.subTest(approval=approval):
                with self.assertRaises(ValueError):
                    self.bundle(Path(td), admission=admission, approval=approval)

    def test_single_run_lower_admission_preserves_stops_and_binds(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            bundle, manifest = self.bundle(root, admission=11776, approval='LAB01')
            self.assertEqual(manifest['preflight_kib'], 11776)
            self.assertEqual(manifest['admission_exception'], {'run_id':'LAB01', 'preflight_kib':11776,
                'default_preflight_kib':12288, 'scope':'single-run upload admission only'})
            self.assertEqual([manifest[k] for k in ('start_after_staging_kib','stop_below_kib','stop_drop_kib','coexist_seconds')], [8192,6144,3072,30])
            gate = json.loads((bundle/'cards/23-client-memory-gate.json').read_text())
            self.assertIn('-ge 11776', gate['command'])
            bound = m.bind_bundle(bundle, self.snapshot(), root/'bound')
            self.assertEqual(bound['admission_exception'], manifest['admission_exception'])
            self.assertFalse(bound['execution_blocked'])
            for key, value in [('preflight_kib',11775),('stop_below_kib',4096),('stop_drop_kib',4096),('start_after_staging_kib',6144)]:
                bad = copy.deepcopy(manifest); bad[key] = value
                (bundle/'manifest.json').write_text(json.dumps(bad))
                with self.subTest(key=key), self.assertRaises(ValueError):
                    m.bind_bundle(bundle, self.snapshot(), root/'bad')
            bad = copy.deepcopy(manifest); bad['admission_exception']['run_id'] = 'OLD_RUN'
            (bundle/'manifest.json').write_text(json.dumps(bad))
            with self.assertRaises(ValueError): m.bind_bundle(bundle, self.snapshot(), root/'bad')

    def test_unbound_bundle_has_no_executable_coexist_or_connect_card(self):
        self.load()
        with tempfile.TemporaryDirectory() as td:
            bundle, manifest = self.bundle(Path(td))
            self.assertTrue(manifest['execution_blocked'])
            self.assertEqual(manifest['preflight_kib'], 12288)
            self.assertFalse(list((bundle/'cards').glob('40-*.json')))
            self.assertIn('coexist.control', json.loads((bundle/'cards/90-clean-owned-files.json').read_text())['command'])

    def test_fresh_binding_generates_only_local_cards_and_preserves_original(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); bundle, _ = self.bundle(root)
            out = root/'bound'; manifest = m.bind_bundle(bundle, self.snapshot(), out)
            self.assertFalse(manifest['device_executed'])
            self.assertFalse(manifest['execution_blocked'])
            self.assertFalse((bundle/'cards/40-coexist.json').exists())
            command = json.loads((out/'cards/40-coexist.json').read_text())
            self.assertIn(' coexist /run/qdc-tun-LAB01 LAB01_COEX 30 ', command['command'])
            self.assertEqual(command['ready'], 'LOGGER_READY LAB01_COEX')
            self.assertLess(manifest['stages'].index('29-1-control-assemble'), manifest['stages'].index('30-load-tun'))
            self.assertEqual((out/'payload/coexist.control.part000').stat().st_mode & 0o777, 0o600)
            for card in (out/'cards').glob('*.json'):
                q = json.loads(card.read_text()); self.assertLessEqual(len(q.get('command', '')), 3300)
                if q['op'] == 'push': self.assertTrue(q['local'].startswith(str(out)))

    def test_binding_refuses_stale_future_reused_or_unsafe_identities(self):
        m = self.load(); good = self.snapshot()
        bad = []
        for delta in (-61, 6):
            s = copy.deepcopy(good); s['observed_at'] = (datetime.now(timezone.utc)+timedelta(seconds=delta)).isoformat(); bad.append(s)
        for path, value in [(('supervisor', 'pid'), 0), (('guardian', 'pid'), 431),
                            (('guardian', 'exe'), '/run/old/guard'), (('supervisor', 'state'), 'S'),
                            (('original_sms', 'start'), True)]:
            s = copy.deepcopy(good); s[path[0]][path[1]] = value; bad.append(s)
        for key, value in [('http_address', '8.8.8.8'), ('original_sms_gone', False), ('boot_id', 'bad')]:
            s = copy.deepcopy(good); s[key] = value; bad.append(s)
        for s in bad:
            with self.subTest(s=s), self.assertRaises(ValueError): m.control_bytes('LAB01', s)

    def test_corrupt_payload_and_unexpected_symlink_refused_before_copy(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); bundle, _ = self.bundle(root)
            part = bundle/'payload/charon.part000'; original = part.read_bytes(); part.write_bytes(b'corrupt')
            with self.assertRaises(ValueError): m.bind_bundle(bundle, self.snapshot(), root/'bad')
            self.assertFalse((root/'bad').exists())
            part.write_bytes(original); (bundle/'unexpected').symlink_to('/etc/passwd')
            with self.assertRaises(ValueError): m.bind_bundle(bundle, self.snapshot(), root/'bad')

    def test_manifest_addresses_and_artifact_names_are_revalidated(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); bundle, manifest = self.bundle(root)
            variants = []
            for key, value in [('target', '10.99.0.1;reboot'), ('vip', '127.0.0.1'),
                               ('target', '10.88.0.2'), ('peer_address', '8.8.8.8\nreboot'),
                               ('transport', 'mac-udp-relay')]:
                bad = copy.deepcopy(manifest); bad['connection'][key] = value; variants.append(bad)
            bad = copy.deepcopy(manifest); bad['artifacts']['../escape'] = bad['artifacts'].pop('charon'); variants.append(bad)
            for bad in variants:
                (bundle/'manifest.json').write_text(json.dumps(bad))
                with self.subTest(connection=bad['connection']), self.assertRaises(ValueError):
                    m.bind_bundle(bundle, self.snapshot(), root/'bad')
                self.assertFalse((root/'bad').exists())

    @unittest.skipUnless(Path('/proc/self/exe').exists(), 'Linux proc identity test')
    def test_cleanup_refuses_a_live_owned_executable(self):
        self.load()
        from prepare import RAM_CHECK
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); bundle, manifest = self.bundle(root)
            ram = root/'ram'; ram.mkdir(); (ram/'RUN_ID').write_text('LAB01\n')
            helper = ram/'qdc-ike-guard'; shutil.copy2(shutil.which('sleep'), helper)
            child = subprocess.Popen([str(helper), '20'])
            try:
                deadline = time.monotonic()+1
                while time.monotonic() < deadline:
                    if os.readlink(f'/proc/{child.pid}/exe') == str(helper): break
                    time.sleep(.01)
                if os.readlink(f'/proc/{child.pid}/exe') != str(helper):
                    self.skipTest('binfmt exposes the interpreter instead of the copied executable')
                command = json.loads((bundle/'cards/90-clean-owned-files.json').read_text())['command']
                command = command.replace(RAM_CHECK, '').replace(manifest['ram_root'], str(ram))
                command = command.replace('test ! -e /sys/module/tun\n', '').replace('test ! -e /run/qdc-ike\n', '')
                result = subprocess.run(['sh', '-c', command], capture_output=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'OWNED_PROCESS_ALIVE', result.stdout)
                self.assertTrue(helper.exists()); self.assertTrue((ram/'RUN_ID').exists())
            finally:
                child.terminate(); child.wait(timeout=3)

    def test_receipt_rejects_old_guard_and_incomplete_arm_evidence(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            local = Path(td); out = local/'coexist-build'; out.mkdir()
            (out/'qdc-ike-guard').write_bytes(b'new-guard')
            digest = hashlib.sha256(b'new-guard').hexdigest()
            (out/'build.json').write_text(json.dumps({'guard_sha256': '0'*64, 'source_sha256': {}}))
            with self.assertRaises(ValueError): m.publish_receipt(local, out, local/'missing', local/'missing')
            (out/'build.json').write_text(json.dumps({'guard_sha256': digest, 'source_sha256': {}}))
            (local/'runtime-build').mkdir(); (local/'output').mkdir()
            for path in (local/'runtime-build/lab-only-runtime-zImage', out/'lab-only-coexist-rootfs.cpio.gz',
                         out/'lab-only-coexist-fixture', local/'output/charon', local/'output/swanctl'):
                path.write_bytes(b'lab-only')
            from test_rootfs_receipt import archive
            for name in ('lab-only-coexist-rootfs.cpio.gz', 'lab-only-runtime-rootfs.cpio.gz'):
                (out/name).write_bytes(archive([('bin/qdc-ike-guard', b'old-guard')]))
            with self.assertRaisesRegex(ValueError, 'embeds another guard'):
                m.publish_receipt(local, out, local/'missing', local/'missing')
            for name in ('lab-only-coexist-rootfs.cpio.gz', 'lab-only-runtime-rootfs.cpio.gz'):
                (out/name).write_bytes(archive([('bin/qdc-ike-guard', b'new-guard')]))
            logs = local/'logs'; logs.mkdir()
            (logs/'acceptance.json').write_text(json.dumps({'status':'passed', 'guard_sha256':digest,
                                                           'cases':['normal_30s'], 'results':[]}))
            with self.assertRaisesRegex(ValueError, 'all seven'):
                m.publish_receipt(local, out, logs, local/'missing')
            self.assertFalse(list(out.glob('coexist-*.json')))


if __name__ == '__main__': unittest.main()
