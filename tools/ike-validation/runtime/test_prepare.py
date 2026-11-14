"""Offline behavior tests. No USB libraries or device commands are used."""
import hashlib
import importlib.util
import json
import subprocess
import inspect
import shutil
from pathlib import Path
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class BundleTests(unittest.TestCase):
    def load(self):
        self.assertTrue((HERE/'prepare.py').exists(), 'offline preparer is not implemented')
        spec = importlib.util.spec_from_file_location('prepare', HERE/'prepare.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_split_is_lossless_and_bounded(self):
        m = self.load()
        blob = bytes(range(256))*10000
        chunks = m.split_blob(blob)
        self.assertEqual(b''.join(chunks), blob)
        self.assertTrue(all(0 < len(x) <= 1048576 for x in chunks))

    def test_compact_probe_requires_matching_vm_receipt(self):
        m = self.load()
        self.assertTrue(hasattr(m, 'probe_input'), 'compact probe selection is not implemented')
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); (root/'runtime-build').mkdir()
            build = {'compact_probe': {'bytes': 60000, 'sha256': 'a'*64}}
            legacy, pin = m.probe_input(root, root/'legacy', build, compact=False)
            self.assertEqual(legacy, root/'legacy/runtime-build/qdc-tun-probe')
            self.assertEqual(pin, m.PINS['qdc-tun-probe'])
            with self.assertRaises(ValueError):
                m.probe_input(root, root/'legacy', build, compact=True)
            receipt = root/'runtime-build'/('compact-probe-'+'a'*64+'.json')
            receipt.write_text(json.dumps({'status':'passed','probe_sha256':'b'*64,'cases':list(m.PROBE_VM_CASES)}))
            with self.assertRaises(ValueError):
                m.probe_input(root, root/'legacy', build, compact=True)
            receipt.write_text(json.dumps({'status':'passed','probe_sha256':'a'*64,'cases':list(m.PROBE_VM_CASES)}))
            probe, pin = m.probe_input(root, root/'legacy', build, compact=True)
            self.assertEqual(probe, root/'runtime-build/qdc-tun-probe-musl')
            self.assertEqual(pin, 'a'*64)

    def test_ids_reject_shell_and_path_injection(self):
        m = self.load()
        for value in ['', '../x', 'a b', 'a;reboot', '$(id)', 'a'*49]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                m.checked_id(value)
        self.assertEqual(m.checked_id('IKE20261113_01'), 'IKE20261113_01')

    def fixture(self, root):
        sources = {}
        for name in ['charon', 'swanctl', 'tun.ko', 'qdc-tun-probe', 'qdc-ike-guard']:
            p = root/name
            p.write_bytes((name.encode()+b'\n')*160000 if name == 'charon' else name.encode())
            sources[name] = p
        return sources, {n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in sources.items()}

    def test_bundle_cards_checksums_and_no_execution(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sources, pins = self.fixture(root)
            out = root/'bundle'
            m.build_bundle(sources, pins, out, 'OFFLINE01')
            manifest = json.loads((out/'manifest.json').read_text())
            self.assertFalse(manifest['device_executed'])
            self.assertEqual(manifest['run_id'], 'OFFLINE01')
            self.assertEqual(manifest['artifacts']['charon']['sha256'], pins['charon'])
            self.assertTrue((out/'cards'/'00-readonly.json').exists())
            self.assertTrue((out/'cards'/'40-idle-10s.json').exists())
            rebuilt = b''.join(p.read_bytes() for p in sorted((out/'payload').glob('charon.part*')))
            self.assertEqual(rebuilt, sources['charon'].read_bytes())
            for card in (out/'cards').glob('*.json'):
                request = json.loads(card.read_text())
                self.assertNotIn('vid', request)
                self.assertLessEqual(len(request.get('command', '')), 3300)
                self.assertLessEqual(request['seconds'], 90)
                self.assertEqual(card.stat().st_mode & 0o777, 0o600)
            self.assertFalse(list(out.glob('*.sh')), 'bundle root must not be an execute-all script')
            stages = manifest['stages']
            self.assertLess(stages.index('22-log-canary-3'), stages.index('23-client-memory-gate'))
            self.assertIn('85-release-client-files', stages)
            self.assertLess(stages.index('85-release-client-files'), stages.index('86-post-restore-log'))
            self.assertLess(stages.index('86-post-restore-log'), stages.index('90-clean-owned-files'))
            with self.assertRaises(FileExistsError):
                m.build_bundle(sources, pins, out, 'OFFLINE01')

    def test_bad_hash_leaves_no_bundle(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sources, pins = self.fixture(root)
            pins['charon'] = '0'*64
            with self.assertRaises(ValueError):
                m.build_bundle(sources, pins, root/'out', 'OFFLINE01')
            self.assertFalse((root/'out').exists())

    def test_symlink_input_is_rejected(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sources, pins = self.fixture(root)
            link = root/'link'
            link.symlink_to(sources['charon'])
            sources['charon'] = link
            with self.assertRaises(ValueError):
                m.build_bundle(sources, pins, root/'out', 'OFFLINE01')

    def test_unknown_cleanup_file_preserves_owner_and_evidence(self):
        m = self.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sources, pins = self.fixture(root)
            out = root/'bundle'
            manifest = m.build_bundle(sources, pins, out, 'OFFLINE01')
            ram = root/'fake-ram'; ram.mkdir()
            (ram/'RUN_ID').write_text('OFFLINE01\n')
            (ram/'charon').write_text('owned test fixture')
            (ram/'unexpected').write_text('retain evidence')
            command = json.loads((out/'cards'/'90-clean-owned-files.json').read_text())['command']
            # Exercise only local filesystem cleanup, never RAM or module actions.
            command = command.replace(m.RAM_CHECK, '').replace(manifest['ram_root'], str(ram))
            command = command.replace('test ! -e /sys/module/tun\n', '').replace('test ! -e /run/qdc-ike\n', '')
            result = subprocess.run(['sh', '-c', command], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue((ram/'RUN_ID').exists(), 'unknown file must preserve ownership evidence')
            self.assertTrue((ram/'charon').exists(), 'reject unknown files before deleting known files')
            (ram/'unexpected').unlink()
            result = subprocess.run(['sh', '-c', command], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(ram.exists())

    def test_low_copy_assembly_preserves_inode_and_verified_bytes(self):
        m = self.load()
        self.assertIn('low_copy', inspect.signature(m.build_bundle).parameters,
                      'move/append assembly is not implemented')
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); sources,pins=self.fixture(root); out=root/'bundle'
            manifest=m.build_bundle(sources,pins,out,'LOWCOPY01',low_copy=True)
            ram=root/'ram';ram.mkdir();(ram/'RUN_ID').write_text('LOWCOPY01\n')
            for name,index in [('charon',2),('swanctl',3)]:
                parts=manifest['artifacts'][name]['parts']
                for part in parts: shutil.copyfile(out/'payload'/part,ram/part)
                inode=(ram/parts[0]).stat().st_ino
                command=json.loads((out/'cards'/f'21-{index}-assemble.json').read_text())['command']
                command=command.replace(m.RAM_CHECK,'').replace(manifest['ram_root'],str(ram))
                result=subprocess.run(['sh','-c',command],capture_output=True,timeout=5)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual((ram/name).read_bytes(),sources[name].read_bytes())
                self.assertEqual((ram/name).stat().st_ino,inode,'first part must be moved, not copied')
                self.assertTrue(all(not (ram/p).exists() for p in parts))

    def test_bad_part_is_rejected_before_low_copy_publish(self):
        m = self.load()
        self.assertIn('low_copy',inspect.signature(m.build_bundle).parameters,
                      'move/append assembly is not implemented')
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);sources,pins=self.fixture(root);out=root/'bundle'
            manifest=m.build_bundle(sources,pins,out,'LOWCOPY01',low_copy=True)
            ram=root/'ram';ram.mkdir();(ram/'RUN_ID').write_text('LOWCOPY01\n')
            parts=manifest['artifacts']['charon']['parts']
            for part in parts: shutil.copyfile(out/'payload'/part,ram/part)
            (ram/parts[-1]).write_bytes(b'corrupt-part')
            command=json.loads((out/'cards'/'21-2-assemble.json').read_text())['command']
            command=command.replace(m.RAM_CHECK,'').replace(manifest['ram_root'],str(ram))
            result=subprocess.run(['sh','-c',command],capture_output=True,timeout=5)
            self.assertNotEqual(result.returncode,0)
            self.assertTrue((ram/parts[0]).exists())
            self.assertFalse((ram/'charon').exists())
            self.assertFalse((ram/'charon.new').exists())

    def test_lower_admission_is_explicit_and_requires_low_copy(self):
        m = self.load()
        self.assertIn('client_admission_kib',inspect.signature(m.build_bundle).parameters,
                      'explicit per-bundle admission is not implemented')
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);sources,pins=self.fixture(root)
            with self.assertRaises(ValueError):
                m.build_bundle(sources,pins,root/'bad','LOWCOPY01',client_admission_kib=11776)
            with self.assertRaises(ValueError):
                m.build_bundle(sources,pins,root/'bad','LOWCOPY01',low_copy=True,client_admission_kib=11775)
            out=root/'ok'
            manifest=m.build_bundle(sources,pins,out,'LOWCOPY01',low_copy=True,client_admission_kib=11776)
            self.assertEqual(manifest['preflight_kib'],11776)
            self.assertEqual(manifest['start_after_staging_kib'],8192)
            self.assertEqual(manifest['stop_below_kib'],6144)
            self.assertEqual(manifest['stop_drop_kib'],3072)


if __name__ == '__main__':
    unittest.main()
