"""Offline tests of the parallel FTP transfer pool against an in-memory fake console."""
import ftplib
import hashlib
import io
import pathlib
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from rinstaller import console as consolemod  # noqa: E402
from rinstaller.jobs import Failed  # noqa: E402


class FakeConn(io.BytesIO):
    def __init__(self, server, path):
        super().__init__()
        self.server, self.path = server, path

    def sendall(self, data):
        self.write(data)

    def close(self):
        with self.server.lock:
            data = self.getvalue()
            if self.server.corrupt == self.path:
                data = data[:-1] + b'X'
            self.server.files[self.path] = data
        super().close()


class FakeFTP:
    def __init__(self, server):
        self.server = server

    def voidcmd(self, cmd):
        return '200'

    def voidresp(self):
        return '226'

    def transfercmd(self, cmd):
        verb, path = cmd.split(' ', 1)
        assert verb == 'STOR'
        return FakeConn(self.server, path)

    def retrbinary(self, cmd, callback, blocksize=8192):
        path = cmd.split(' ', 1)[1]
        with self.server.lock:
            if self.server.refuse > 0:
                self.server.refuse -= 1
                raise ftplib.error_perm('550 Cannot open file.')
            data = self.server.files[path]
        for i in range(0, len(data), blocksize):
            callback(data[i:i + blocksize])

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        pass


class FakeConsole:
    def __init__(self):
        self.files, self.lock, self.refuse, self.corrupt = {}, threading.Lock(), 0, None

    def ftp(self, timeout=60):
        return FakeFTP(self)

    remote_sha = staticmethod(consolemod.Console.remote_sha)


class Job:
    def __init__(self):
        self.lines = []

    def log(self, level, text):
        self.lines.append((level, text))

    def check_cancel(self):
        pass


class TransferTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        self.files = []
        for i in range(12):
            data = bytes([i]) * (1000 * (i + 1))
            p = root / f'f{i}.bin'
            p.write_bytes(data)
            self.files.append((f'game/f{i}.bin', p, len(data), hashlib.sha256(data).hexdigest()))
        self.app = {'files': self.files, 'bytes': sum(f[2] for f in self.files)}
        self.nosleep = mock.patch.object(consolemod.time, 'sleep', lambda s: None)  # no back-off in tests
        self.nosleep.start()

    def tearDown(self):
        self.nosleep.stop()
        self.tmp.cleanup()

    def test_upload_then_resume(self):
        c = FakeConsole()
        stats = consolemod.transfer(Job(), c, '/data/homebrew/PPSA88370', self.app, {}, 4)
        self.assertEqual((stats['sent'], stats['kept']), (12, 0))
        sizes = {f'/data/homebrew/PPSA88370/{r}': s for r, _, s, _ in self.files}
        c.files['/data/homebrew/PPSA88370/game/f3.bin'] = b'stale'
        sizes['/data/homebrew/PPSA88370/game/f3.bin'] = 5
        stats = consolemod.transfer(Job(), c, '/data/homebrew/PPSA88370', self.app, sizes, 4)
        self.assertEqual((stats['sent'], stats['kept']), (1, 11))

    def test_retry_on_saturation(self):
        c = FakeConsole()
        c.refuse = 3
        job = Job()
        stats = consolemod.transfer(job, c, '/r', self.app, {}, 3)
        # an upload whose read-back was refused is verified by hash on the retry (counted as identical)
        self.assertEqual(stats['sent'] + stats['kept'], 12)
        for rel, _, _, sha in self.files:
            self.assertEqual(hashlib.sha256(c.files['/r/' + rel]).hexdigest(), sha)
        self.assertTrue(any('retry' in t for _, t in job.lines))

    def test_corruption_fails(self):
        c = FakeConsole()
        c.corrupt = '/r/game/f5.bin'
        with self.assertRaises(Failed):
            consolemod.transfer(Job(), c, '/r', self.app, {}, 2)


class ListdirTests(unittest.TestCase):
    """zftpd answers MLSD on a missing directory with an empty listing instead of an error."""

    class Ftp:
        tree = {'/': {'data': 'dir'}, '/data': {'game': 'dir'}, '/data/game': {}}

        def mlsd(self, path, facts=None):
            for name, kind in self.tree.get(path, {}).items():
                yield name, {'type': kind, 'size': '0'}

    def test_missing_directory_is_none(self):
        f = self.Ftp()
        self.assertEqual(consolemod.Console.listdir(f, '/data/game'), {})
        self.assertIsNone(consolemod.Console.listdir(f, '/data/game/$SystemUpdate'))
        self.assertIsNone(consolemod.Console.listdir(f, '/data/nothing'))
        self.assertEqual(consolemod.Console.listdir(f, '/data'), {'game': {'type': 'dir', 'size': 0, 'mode': ''}})


if __name__ == '__main__':
    unittest.main()
