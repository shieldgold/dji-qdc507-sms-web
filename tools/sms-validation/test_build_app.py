"""Build contract tests on the computer; no compiler download or device access."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BuildAppTests(unittest.TestCase):
    def run_builder(self, version='go1.23.12', explicit=False, profile=None):
        with tempfile.TemporaryDirectory(prefix='qdc-build-contract-') as tmp:
            root = Path(tmp).resolve()
            shutil.copy2(Path(__file__).resolve().parents[2] / 'build.sh', root / 'build.sh')
            bin_dir = root / 'bin'
            bin_dir.mkdir()
            log = root / 'compiler.json'
            fake = '''#!/usr/bin/env python3
import json, os, pathlib, sys
if sys.argv[1:] == ['env', 'GOVERSION']:
 print('go1.23.12' if pathlib.Path(sys.argv[0]).name == 'target-go' else os.environ['FAKE_VERSION'])
elif sys.argv[1] == 'build':
 args=sys.argv[2:]
 pathlib.Path(os.environ['FAKE_LOG']).write_text(json.dumps({'args':args,'cwd':os.getcwd(),'env':{k:os.environ.get(k) for k in ('GOROOT','GOENV','GOTOOLCHAIN','CGO_ENABLED','GOOS','GOARCH','GOARM')}}))
 pathlib.Path(args[args.index('-o')+1]).write_bytes(b'computer-only fake artifact')
else:
 raise SystemExit(2)
'''
            for name in ('go', 'target-go'):
                path = bin_dir / name
                path.write_text(fake)
                path.chmod(0o700)
            output = root / ('explicit artifact with spaces' if explicit else 'smsweb-armv7')
            env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'],
                       FAKE_VERSION=version, FAKE_LOG=str(log), GOROOT='/wrong-go-root',
                       GOENV='/wrong-go-env', GOTOOLCHAIN='auto')
            env.pop('QDC_BUILD_PROFILE', None)
            if profile is not None:
                env['QDC_BUILD_PROFILE'] = profile
            if explicit:
                env.update(QDC_GO=str(bin_dir / 'target-go'), QDC_OUTPUT=str(output))
            result = subprocess.run(['sh', str(root / 'build.sh')], cwd=root.parent,
                                    env=env, capture_output=True, text=True, timeout=5)
            info = json.loads(log.read_text()) if log.exists() else None
            return result, info, output.exists(), str(root)

    def test_incompatible_go_fails_before_compiling(self):
        for version in ('go1.20.6', 'go1.24.0', 'go1.25.6', 'go2.0.0'):
            with self.subTest(version=version):
                result, info, exists, _ = self.run_builder(version)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIsNone(info)
                self.assertFalse(exists)

    def test_compatible_build_isolates_toolchain_and_pins_target(self):
        result, info, exists, root = self.run_builder()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(exists)
        self.assertEqual(info['cwd'], root)
        self.assertIsNone(info['env']['GOROOT'])
        self.assertEqual(info['env']['GOENV'], 'off')
        self.assertEqual(info['env']['GOTOOLCHAIN'], 'local')
        for key, value in {'CGO_ENABLED':'0', 'GOOS':'linux', 'GOARCH':'arm', 'GOARM':'7'}.items():
            self.assertEqual(info['env'][key], value)
        self.assertIn('-trimpath', info['args'])
        self.assertIn('-buildvcs=false', info['args'])
        self.assertNotIn('-gcflags=all=-l', info['args'])

    def test_memory_profile_explicitly_disables_inlining(self):
        result, info, exists, _ = self.run_builder(profile='memory')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(exists)
        self.assertIn('-gcflags=all=-l', info['args'])
        self.assertEqual(info['env']['GOARM'], '7')

    def test_unknown_profile_is_refused_before_compiling(self):
        result, info, exists, _ = self.run_builder(profile='unknown')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIsNone(info)
        self.assertFalse(exists)

    def test_explicit_compatible_compiler_and_output_are_honored(self):
        result, info, exists, _ = self.run_builder('go1.25.6', explicit=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(exists)
        self.assertTrue(info['args'][info['args'].index('-o')+1].endswith('explicit artifact with spaces'))


if __name__ == '__main__':
    unittest.main()
