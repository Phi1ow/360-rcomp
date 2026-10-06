"""tools/install_kit.py on synthetic archives (no kit, no network): links as copies, path guards, hashes, --check."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import install_kit  # noqa: E402


def make_archive(path, members):
    """members: (name, kind, payload): kind 'file' (payload bytes), 'dir', 'sym' or 'hard' (payload: link target)."""
    with tarfile.open(path, 'w:gz') as tar:
        for name, kind, payload in members:
            info = tarfile.TarInfo(name)
            info.mtime = 1_700_000_000
            if kind == 'file':
                info.size = len(payload)
                info.mode = 0o755
                tar.addfile(info, io.BytesIO(payload))
            elif kind == 'dir':
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                tar.addfile(info)
            else:
                info.type = tarfile.SYMTYPE if kind == 'sym' else tarfile.LNKTYPE
                info.linkname = payload
                tar.addfile(info)


def asset_for(path, **extra):
    data = Path(path).read_bytes()
    return {'name': Path(path).name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(), **extra}


class InstallKitTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.root = self.tmp / 'checkout'
        self.root.mkdir()
        self.archives = self.tmp / 'archives'
        self.archives.mkdir()

    def lock(self, *assets):
        path = self.tmp / 'kit.lock.json'
        path.write_text(json.dumps({'repository': 'x/y', 'tag': 't', 'assets': list(assets)}), encoding='utf-8')
        return path

    def run_main(self, lock, *extra):
        out = io.StringIO()
        real = sys.stdout
        sys.stdout = out
        try:
            code = install_kit.main(['--lock', str(lock), '--root', str(self.root), *extra])
        finally:
            sys.stdout = real
        return code, out.getvalue()

    def test_links_become_copies(self):
        a = self.archives / 'kit.tar.gz'
        make_archive(a, [
            ('build/sdk/target/include', 'dir', None),
            ('build/sdk/target/include/sys/errno.h', 'file', b'#define EINVAL 22\n'),
            ('build/sdk/target/include/errno.h', 'sym', 'sys/errno.h'),          # relative to its folder
            ('build/sdk/target/include/again.h', 'sym', 'errno.h'),               # a link to a link
            ('build/sdk/target/include_common', 'sym', 'include'),                # a link to a folder
            ('build/sdk/target/lib/copy.h', 'hard', 'build/sdk/target/include/sys/errno.h'),
        ])
        code, out = self.run_main(self.lock(asset_for(a, provides=['build/sdk/target/include/errno.h'])), '--from', str(self.archives))
        self.assertEqual(code, 0, out)
        inc = self.root / 'build/sdk/target/include'
        for name in ('errno.h', 'again.h', 'sys/errno.h'):
            self.assertEqual((inc / name).read_bytes(), b'#define EINVAL 22\n', name)
            self.assertFalse((inc / name).is_symlink(), name)
        self.assertEqual((self.root / 'build/sdk/target/include_common/errno.h').read_bytes(), b'#define EINVAL 22\n')
        self.assertEqual((self.root / 'build/sdk/target/lib/copy.h').read_bytes(), b'#define EINVAL 22\n')

    def test_members_outside_build_are_refused(self):
        for bad in ('../evil', '/abs/evil', 'build/../../evil', 'C:/evil', 'other/file', 'build\\win\\file', ''):
            with self.assertRaises(install_kit.KitError, msg=bad):
                install_kit.checked_member_path(bad)
        self.assertEqual(install_kit.checked_member_path('./build/a//b/./c'), 'build/a/b/c')

    def test_bad_links_are_refused(self):
        for target in ('/etc/passwd', '../../../x', '../../outside.h', 'C:/x'):
            a = self.archives / 'bad.tar.gz'
            make_archive(a, [('build/a/l.h', 'sym', target)])
            code, out = self.run_main(self.lock(asset_for(a)), '--from', str(self.archives))
            self.assertEqual(code, 2, (target, out))
            self.assertFalse((self.root / 'build/a/l.h').exists(), target)

    def test_unresolved_link_fails(self):
        a = self.archives / 'dangling.tar.gz'
        make_archive(a, [('build/a/l.h', 'sym', 'missing.h')])
        code, out = self.run_main(self.lock(asset_for(a)), '--from', str(self.archives))
        self.assertEqual(code, 2)
        self.assertIn('not in the archives', out)

    def test_hash_mismatch_extracts_nothing(self):
        a = self.archives / 'kit.tar.gz'
        make_archive(a, [('build/a/file', 'file', b'x')])
        asset = asset_for(a)
        asset['sha256'] = '0' * 64
        code, out = self.run_main(self.lock(asset), '--from', str(self.archives))
        self.assertEqual(code, 2)
        self.assertIn('does not match the lock', out)
        self.assertFalse((self.root / 'build').exists())

    def test_check_reports_missing_then_present(self):
        a = self.archives / 'kit.tar.gz'
        make_archive(a, [('build/a/file', 'file', b'x')])
        lock = self.lock(asset_for(a, provides=['build/a/file', 'build/a/other']))
        code, out = self.run_main(lock, '--check')
        self.assertEqual(code, 1)
        self.assertIn('MISSING build/a/file', out)
        code, out = self.run_main(lock, '--from', str(self.archives))
        self.assertEqual(code, 1, out)  # build/a/other is not in the archive: the final check says so
        self.assertIn('MISSING build/a/other', out)
        self.assertEqual(self.run_main(self.lock(asset_for(a, provides=['build/a/file'])), '--check')[0], 0)

    def test_replace_removes_an_older_tree(self):
        stale = self.root / 'build/x/src/old_only.cpp'
        stale.parent.mkdir(parents=True)
        stale.write_text('old', encoding='utf-8')
        a = self.archives / 'xenos.tar.gz'
        make_archive(a, [('build/x/src/new.cpp', 'file', b'new')])
        code, out = self.run_main(self.lock(asset_for(a, replace=['build/x/src'], provides=['build/x/src/new.cpp'])),
                                  '--from', str(self.archives))
        self.assertEqual(code, 0, out)
        self.assertFalse(stale.exists())
        self.assertTrue((self.root / 'build/x/src/new.cpp').is_file())

    def test_replace_cannot_leave_build(self):
        a = self.archives / 'kit.tar.gz'
        make_archive(a, [('build/a/file', 'file', b'x')])
        victim = self.root / 'tools'
        victim.mkdir()
        code, out = self.run_main(self.lock(asset_for(a, replace=['tools'])), '--from', str(self.archives))
        self.assertEqual(code, 2)
        self.assertTrue(victim.is_dir())


if __name__ == '__main__':
    unittest.main()
