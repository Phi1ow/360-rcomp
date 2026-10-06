"""'Check a game' logic: kernel version rule, service categories, verdicts and the work order (no disc)."""
import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from rinstaller import compat  # noqa: E402


def xex_with_imports(libraries):
    """A minimal XEX2 header whose import library table (key 0x103FF) lists (name, version, minimum)."""
    strtab = b''.join((n.encode() + b'\0').ljust((len(n) + 1 + 3) & ~3, b'\0') for n, _, _ in libraries)
    libs = b''
    for i, (_, version, minimum) in enumerate(libraries):
        lib = bytearray(0x28)
        struct.pack_into('>I', lib, 0, 0x28)
        struct.pack_into('>II', lib, 0x1C, version, minimum)
        struct.pack_into('>HH', lib, 0x24, i, 0)
        libs += bytes(lib)
    table = struct.pack('>III', 12 + len(strtab) + len(libs), len(strtab), len(libraries)) + strtab + libs
    header = bytearray(0x200)
    header[0:4] = b'XEX2'
    struct.pack_into('>I', header, 8, 0x1000)
    struct.pack_into('>I', header, 20, 1)
    struct.pack_into('>II', header, 24, 0x000103FF, 0x100)
    header[0x100:0x100 + len(table)] = table
    return bytes(header)


def packed(build):
    return 0x20000000 | (build << 8)


class KernelRuleTests(unittest.TestCase):
    def check(self, libraries):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / 'plain.xex'
            p.write_bytes(xex_with_imports(libraries))
            return compat.kernel_check(p)

    def test_title_own_version(self):
        r = self.check([('xboxkrnl.exe', packed(7645), packed(6717)), ('xam.xex', packed(7645), packed(6717))])
        self.assertEqual(r['status'], 'PASS')
        self.assertEqual(r['kernel'], '2.0.7645.0')
        self.assertEqual(self.check([('xboxkrnl.exe', packed(6683), packed(6683))])['status'], 'PASS')

    def test_later_xdk_sees_final_kernel(self):
        # NARUTO STORM 3: built against XDK 2.0.21173, minimum 2.0.x retail: runs, sees 2.0.17559 (runtime rule).
        r = self.check([('xboxkrnl.exe', packed(21173), packed(6683))])
        self.assertEqual(r['status'], 'PASS')
        self.assertEqual(r['kernel'], '2.0.17559.0')

    def test_refusals(self):
        self.assertEqual(self.check([('xboxkrnl.exe', packed(6000), packed(6683))])['status'], 'BLOCKED')
        self.assertEqual(self.check([('xboxkrnl.exe', packed(21173), packed(18000))])['status'], 'BLOCKED')
        self.assertEqual(self.check([('xboxkrnl.exe', packed(7645), packed(6717)),
                                     ('xam.xex', packed(8000), packed(6717))])['status'], 'BLOCKED')
        self.assertEqual(self.check([('xam.xex', packed(7645), packed(6717))])['status'], 'BLOCKED')


class ReportTests(unittest.TestCase):
    def test_categories(self):
        self.assertEqual(compat.category('XamContentCreateEx'), 'Saves and storage')
        self.assertEqual(compat.category('NetDll_XNetQosLookup'), 'Xbox LIVE and network')
        self.assertEqual(compat.category('XamShowMessageBoxUIEx'), 'System dialogs')
        self.assertEqual(compat.category('RtlUnwind'), 'Exceptions (SEH)')
        self.assertEqual(compat.category('XamNuiCameraGetTiltControl'), 'Kinect')
        self.assertEqual(compat.category('NtPulseEvent'), 'Threads, sync and memory')

    def report(self, imports, xenonrecomp=None):
        inv = {'imports': imports, 'xenonrecomp': xenonrecomp or {'ran': True, 'exit_code': 0, 'functions': 10},
               'decode': {'supported': True}}
        with tempfile.TemporaryDirectory() as d:
            return compat.build_report(inv, pathlib.Path(d), pathlib.Path(d), ['default.xex'], 'game.iso', 'x')

    def test_verdicts(self):
        ready = self.report({'xboxkrnl.exe': {'functions_total': 2, 'functions_implemented': ['A', 'B'],
                                              'functions_missing': [], 'variables_missing': []}})
        # No decoded image in this unit test: the kernel rule cannot pass, so the verdict is BLOCKED.
        self.assertEqual(ready['verdict'], 'BLOCKED')
        self.assertIn('kernel version', ready['blockers'][0])
        try_it = self.report({'xam.xex': {'functions_total': 2, 'functions_implemented': ['A'],
                                          'functions_missing': ['XamVoiceCreate'], 'variables_missing': []}})
        self.assertEqual(try_it['missing_by_category'], {'Voice': ['XamVoiceCreate (xam.xex)']})
        order = compat.work_order(try_it)
        self.assertIn('# R-comp support work order', order)
        self.assertIn('`XamVoiceCreate (xam.xex)`', order)
        generator = self.report({}, {'ran': True, 'exit_code': 0, 'warnings': {'unimplemented': 3}})
        self.assertTrue(any('XenonRecomp' in b for b in generator['blockers']))


class RunOutcomeTests(unittest.TestCase):
    LOG_OLD = 'RCOMP-TITLE begin title=PPSA88370 pid=1\nold run\n'

    def test_missing_import_is_named(self):
        log = self.LOG_OLD + 'RCOMP-TITLE begin title=PPSA88370 pid=2\nRCOMP-APP stage=guest\n'
        err = ('RCOMP-FPS fps=30.00 frame_ms=33\nRCOMP-FATAL kind=missing_import module=xam.xex ordinal=0x02BA '
               'name=XamShowFriendsUI lr=0x82000000 r3=0x0 r4=0x0 r5=0x0 r6=0x0\n')
        lines = compat.last_run_lines(log, err, len(self.LOG_OLD))
        out = compat.judge(lines, False, 42)
        self.assertEqual(out['result'], 'stopped')
        self.assertEqual(out['missing_import'], {'module': 'xam.xex', 'ordinal': '0x02BA', 'name': 'XamShowFriendsUI'})
        self.assertEqual(out['fps_last'], 30.0)

    def test_no_new_run(self):
        self.assertIsNone(compat.last_run_lines(self.LOG_OLD, 'stale err', len(self.LOG_OLD)))

    def test_running_crash_exit(self):
        log = 'RCOMP-TITLE begin title=X\n'
        self.assertEqual(compat.judge(compat.last_run_lines(log, 'RCOMP-FPS fps=57.10 x\n'), True, 180)['result'], 'running')
        self.assertEqual(compat.judge(compat.last_run_lines(log, 'RCOMP-CRASH signal=11 rip=0x0\n'), False, 5)['result'],
                         'crashed')
        self.assertEqual(compat.judge(compat.last_run_lines(log + 'RCOMP-TITLE end status=3\n', ''), True, 5)['result'],
                         'exited')
        self.assertEqual(compat.judge(compat.last_run_lines(log, ''), False, 5)['result'], 'vanished')


class BacklogTests(unittest.TestCase):
    def report(self, check, name, missing, verdict='TRY IT'):
        return {'check': check, 'source': f'C:/isos/{name}.iso', 'name': name, 'title_id': 'T' + name,
                'media_id': 'M', 'disc': '1/1', 'modules': ['default.xex'], 'profile': 'generic',
                'profile_label': 'Generic', 'profile_reason': '', 'functions': 10,
                'generator': {'warnings': {}, 'unrecognized': {}},
                'kernel': {'status': 'PASS', 'reason': 'ok'}, 'services': {},
                'missing_by_category': {'Xbox LIVE and network': [f'{m} (xam.xex)' for m in missing]},
                'missing_functions': len(missing), 'missing_variables': [], 'blockers': [], 'verdict': verdict,
                'verdict_text': 'v', 'files_dir': '', 'checked': '2026-10-03 10:00'}

    def test_ranking_and_files(self):
        import json
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            for check, name, missing in (('20261003-100000', 'A', ['NetDll_send', 'NetDll_recv']),
                                         ('20261003-100001', 'B', ['NetDll_send']),
                                         ('20261003-100002', 'C', ['XNetStartup'])):
                (root / check).mkdir()
                (root / check / 'report.json').write_text(json.dumps(self.report(check, name, missing)))
            compat.record_try(root, '20261003-100002', compat.judge(
                ['RCOMP-TITLE begin', 'RCOMP-FATAL kind=missing_import module=xam.xex ordinal=0x0033 name=XNetStartup '
                 'lr=0x0 r3=0x0 r4=0x0 r5=0x0 r6=0x0'], False, 3))
            data = json.loads((root / 'backlog.json').read_text())
            names = [f['name'] for f in data['functions']]
            self.assertEqual(names[0], 'XNetStartup (xam.xex)')  # really called on the PS5 first
            self.assertEqual(names[1], 'NetDll_send (xam.xex)')  # then by number of games
            md = (root / 'BACKLOG.md').read_text()
            self.assertIn('Priority 1', md)
            self.assertIn('stopped C', md)
            self.assertIn('PS5 tries', (root / '20261003-100002' / 'WORK_ORDER.md').read_text())
            self.assertEqual(len(compat.checked_sources(root)), 3)


if __name__ == '__main__':
    unittest.main()
