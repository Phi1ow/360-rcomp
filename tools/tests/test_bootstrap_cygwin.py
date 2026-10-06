"""The stand-in scripts and the CA bundle step of tools/bootstrap_cygwin.py (no download, no Cygwin needed)."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import bootstrap_cygwin as bc  # noqa: E402

LOCK = {'packages': [{'name': 'bash', 'version': '5.2.21-1'}, {'name': 'llvm', 'version': '22.1.8-2'}]}


class StandInScriptsTest(unittest.TestCase):
    def test_llvm_version_comes_from_the_locked_llvm_package(self):
        self.assertEqual(bc.llvm_version(LOCK), '22.1.8')
        with self.assertRaises(ValueError):
            bc.llvm_version({'packages': [{'name': 'bash', 'version': '5-1'}]})

    def test_the_real_lock_has_an_llvm_package(self):
        import json
        lock = json.loads((TOOLS.parent / 'deps' / 'cygwin-host.lock.json').read_text(encoding='utf-8'))
        self.assertRegex(bc.llvm_version(lock), r'^\d+\.\d+\.\d+$')

    def test_scripts_are_lf_bash_scripts_with_the_options_the_sdk_wrappers_use(self):
        shims = bc.sdk_shims(LOCK)
        self.assertEqual(sorted(shims), ['bin/llvm-config', 'bin/rev'])
        for rel, text in shims.items():
            self.assertTrue(text.startswith('#!/usr/bin/bash\n'), rel)
            self.assertNotIn('\r', text, rel)
        cfg = shims['bin/llvm-config']
        self.assertIn('--version) echo 22.1.8 ;;', cfg)  # the wrappers test `version >= 20` to skip the crt objects
        self.assertIn('--bindir) echo /usr/bin ;;', cfg)

    def test_write_shims_writes_lf_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'bin').mkdir()
            names = bc.write_shims(root, LOCK)
            self.assertEqual(names, ['bin/llvm-config', 'bin/rev'])
            for rel in names:
                self.assertNotIn(b'\r', (root / rel).read_bytes())

    @unittest.skipUnless(shutil.which('bash') and shutil.which('gawk'), 'needs bash and gawk on PATH')
    def test_scripts_behave_like_the_tools_they_replace(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'bin').mkdir()
            bc.write_shims(root, LOCK)
            cfg, rev = str(root / 'bin/llvm-config'), str(root / 'bin/rev')

            def run(*cmd, **kw):
                return subprocess.run(['bash', *cmd], capture_output=True, text=True, **kw)

            self.assertEqual(run(cfg, '--version').stdout.strip(), '22.1.8')
            self.assertEqual(run(cfg, '--bindir').stdout.strip(), '/usr/bin')
            self.assertNotEqual(run(cfg, '--cxxflags').returncode, 0)
            # the SDK wrapper's idiom: the tool name after its last '-'
            out = run('-c', f'echo prospero-llvm-config | bash "{rev}" | cut -d- -f1 | bash "{rev}"')
            self.assertEqual(out.stdout.strip(), 'config')
            self.assertEqual(run(rev, input='abc\nxy\n').stdout.split(), ['cba', 'yx'])


class CaBundleTest(unittest.TestCase):
    def fake_run(self, root, rc, content):
        def run(cmd, **kw):
            bundle = root / 'etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem'
            bundle.parent.mkdir(parents=True, exist_ok=True)
            bundle.write_bytes(content)
            return subprocess.CompletedProcess(cmd, rc, '', '')
        return run

    def test_bundle_is_copied_where_git_and_curl_look(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with mock.patch.object(bc.subprocess, 'run', self.fake_run(root, 0, b'-----BEGIN CERTIFICATE-----\n')):
                self.assertTrue(bc.install_ca_bundle(root))
            for rel in ('etc/pki/tls/certs/ca-bundle.crt', 'etc/pki/tls/cert.pem'):
                self.assertEqual((root / rel).read_bytes(), b'-----BEGIN CERTIFICATE-----\n')

    def test_failures_are_reported_not_raised(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with mock.patch.object(bc.subprocess, 'run', self.fake_run(root, 1, b'x')):
                self.assertFalse(bc.install_ca_bundle(root))
            with mock.patch.object(bc.subprocess, 'run', self.fake_run(root, 0, b'')):  # the empty placeholder of the package
                self.assertFalse(bc.install_ca_bundle(root))
            self.assertFalse(bc.install_ca_bundle(root / 'no-such-cygwin'))  # bash.exe missing


if __name__ == '__main__':
    unittest.main()
