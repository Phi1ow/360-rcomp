"""Exercise the runner's actual Python functions without network connections."""
import ast
import ftplib
import pathlib
import re
import socket
import tempfile
import threading
import types
import unittest


RUNNER = pathlib.Path(__file__).resolve().parents[1] / "ps5/tools/run_title.sh"
SOURCE = RUNNER.read_text().split("exec python3 - <<'PY'\n", 1)[1].rsplit("\nPY", 1)[0]
TREE = ast.parse(SOURCE, filename=str(RUNNER))
FUNCTIONS = ast.Module(body=[node for node in TREE.body if isinstance(node, ast.FunctionDef)
                            and node.name in {"capture", "stop_capture", "ctl",
                                              "clear_stale_title_log", "last_title_run"}], type_ignores=[])


class TESTDOUBLE_socket:
    def __init__(self, payload):
        self.payload = payload
        self.waiting = threading.Event()
        self.closed = False
        self.timeout = None
        self.sent = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.closed = True

    def settimeout(self, timeout):
        self.timeout = timeout

    def sendall(self, data):
        self.sent = data

    def recv(self, _):
        if self.payload is not None:
            payload, self.payload = self.payload, None
            return payload
        self.waiting.set()
        threading.Event().wait(0.01)
        raise socket.timeout()


class TESTDOUBLE_ftp:
    def __init__(self, names, delete_error=None, list_error=None):
        self.names = names
        self.delete_error = delete_error
        self.list_error = list_error
        self.deleted = None
        self.listed = None

    def delete(self, path):
        self.deleted = path
        if self.delete_error is not None:
            raise self.delete_error

    def nlst(self, directory):
        self.listed = directory
        if self.list_error is not None:
            raise self.list_error
        return self.names


class RunnerCaptureTests(unittest.TestCase):
    def namespace(self, connection, logdir):
        def connect(address, timeout):
            self.assertEqual(address[0], "TESTDOUBLE_no_network")
            self.assertIn(timeout, (10, 15))
            return connection

        namespace = {"socket": types.SimpleNamespace(create_connection=connect, timeout=socket.timeout),
                     "ftplib": ftplib, "pathlib": pathlib, "re": re,
                     "host": "TESTDOUBLE_no_network", "ctl_port": 1, "klog_port": 2,
                     "logdir": pathlib.Path(logdir), "stop": threading.Event()}
        exec(compile(FUNCTIONS, str(RUNNER), "exec"), namespace)
        return namespace

    def test_short_capture_flushed_before_shutdown_and_thread_joined(self):
        with tempfile.TemporaryDirectory() as directory:
            payload = b"short klog tail\n"
            connection = TESTDOUBLE_socket(payload)
            namespace = self.namespace(connection, directory)
            thread = threading.Thread(target=namespace["capture"], daemon=True)
            namespace["capture_thread"] = thread
            thread.start()
            try:
                self.assertTrue(connection.waiting.wait(2))
                self.assertTrue(thread.is_alive())
                self.assertEqual((pathlib.Path(directory) / "klog.txt").read_bytes(), payload)
            finally:
                namespace["stop_capture"]()
            self.assertFalse(thread.is_alive())
            self.assertTrue(connection.closed)
            self.assertFalse((pathlib.Path(directory) / "klog.error").exists())

    def test_control_read_timeout_covers_launch_polling(self):
        with tempfile.TemporaryDirectory() as directory:
            connection = TESTDOUBLE_socket(b"ok idle\n")
            namespace = self.namespace(connection, directory)
            self.assertEqual(namespace["ctl"]("procs"), "ok idle")
            self.assertEqual(connection.timeout, 30)
            self.assertEqual(connection.sent, b"procs\n")
            self.assertTrue(connection.closed)

    def test_latest_partial_run_cannot_reuse_old_success(self):
        parse = self.namespace(None, ".")["last_title_run"]
        old = "RCOMP-TITLE begin title=old\nRCOMP-TITLE end status=0\n"
        partial = 'RCOMP-TITLE begin title=new\n{"reason":"signal 11"}\n'
        current, status = parse(old + partial)
        self.assertEqual(current, partial)
        self.assertIsNone(status)

    def test_last_run_requires_begin_and_complete_end_line(self):
        parse = self.namespace(None, ".")["last_title_run"]
        for text in ("", "RCOMP-TITLE end status=0\n", "RCOMP-TITLE begin\n",
                     "RCOMP-TITLE begin\nRCOMP-TITLE end status=0truncated\n"):
            with self.subTest(text=text):
                self.assertIsNone(parse(text)[1])

    def test_last_run_status_ignores_earlier_run(self):
        parse = self.namespace(None, ".")["last_title_run"]
        for earlier, latest in ((0, 7), (9, 0)):
            text = (f"RCOMP-TITLE begin title=old\r\nRCOMP-TITLE end status={earlier}\r\n"
                    f"RCOMP-TITLE begin title=new\r\nRCOMP-TITLE end status={latest}\r\n")
            with self.subTest(latest=latest):
                self.assertEqual(parse(text)[1], latest)

    def test_stale_log_remains_even_if_delete_claims_success(self):
        clear = self.namespace(None, ".")["clear_stale_title_log"]
        directory = "/data/homebrew/PPSA88360"
        for name in ("rcomp_title.log", directory + "/rcomp_title.log"):
            for error in (None, ftplib.error_perm("550 Permission denied")):
                with self.subTest(name=name, error=error):
                    ftp = TESTDOUBLE_ftp([name], delete_error=error)
                    with self.assertRaisesRegex(RuntimeError, "still present"):
                        clear(ftp, directory)
                    self.assertEqual(ftp.listed, directory)

    def test_stale_log_absence_must_be_verified_by_listing(self):
        clear = self.namespace(None, ".")["clear_stale_title_log"]
        directory = "/data/homebrew/PPSA88360"
        ftp = TESTDOUBLE_ftp([directory + "/eboot.bin"],
                            delete_error=ftplib.error_perm("550 No such file"))
        clear(ftp, directory)
        self.assertEqual(ftp.deleted, directory + "/rcomp_title.log")
        self.assertEqual(ftp.listed, directory)
        for error in (ftplib.error_perm("550 Cannot list directory"), socket.timeout()):
            with self.subTest(error=error):
                ftp = TESTDOUBLE_ftp([], list_error=error)
                with self.assertRaises(type(error)):
                    clear(ftp, directory)


if __name__ == "__main__":
    unittest.main(verbosity=2)
