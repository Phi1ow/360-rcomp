"""tools/setup_windows.py and tools/cygwin_run.py: step order, 'done' checks on a synthetic checkout, failure handling (no network)."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import cygwin_run  # noqa: E402
import setup_windows as sw  # noqa: E402


def touch(root, rel, data=b'x'):
    p = Path(root, rel)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return p


class StepsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        env = mock.patch.dict(os.environ)
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop('RCOMP_CYGWIN_BASH', None)

    def done(self):
        return {s.name: s.done() for s in sw.steps(self.root)}

    def test_order(self):
        self.assertEqual([s.name for s in sw.steps(self.root)],
                         ['sources', 'zstandard', 'cygwin', 'kit', 'extract-xiso', 'xenonrecomp'])

    def test_an_empty_checkout_has_nothing_done(self):
        touch(self.root, 'deps/kit.lock.json', json.dumps({'assets': [{'provides': ['build/a']}]}).encode())
        self.assertFalse(any(self.done().values()))

    def test_sources_and_zstandard_and_extract_xiso(self):
        touch(self.root, 'third_party/XenonRecomp/thirdparty/fmt/CMakeLists.txt')
        touch(self.root, 'build/prime-host-tools/pylibs/zstandard/__init__.py')
        touch(self.root, 'build/catalog-tools/extract-xiso/extract-xiso.exe')
        done = self.done()
        self.assertTrue(done['sources'] and done['zstandard'] and done['extract-xiso'])
        self.assertFalse(done['cygwin'])

    def test_the_bootstrapped_cygwin_counts_only_with_its_completion_marker(self):
        touch(self.root, 'build/prime-host-tools/cygwin/bin/bash.exe')
        touch(self.root, 'build/prime-host-tools/cygwin/bin/llvm-config')  # written before the CA step: not proof of an end
        self.assertFalse(self.done()['cygwin'])
        touch(self.root, 'build/prime-host-tools/cygwin/etc/rcomp-bootstrap-complete')
        self.assertTrue(self.done()['cygwin'])

    def test_a_full_cygwin_needs_llvm_config(self):
        touch(self.root, 'build/vulkan-gta-radv-20260928/host-cygwin-full/bin/bash.exe')
        self.assertFalse(self.done()['cygwin'])
        touch(self.root, 'build/vulkan-gta-radv-20260928/host-cygwin-full/bin/llvm-config.exe')
        self.assertTrue(self.done()['cygwin'])

    def test_kit_follows_the_key_files_of_the_lock(self):
        touch(self.root, 'deps/kit.lock.json', json.dumps({'assets': [{'provides': ['build/a', 'build/b']}]}).encode())
        touch(self.root, 'build/a')
        self.assertFalse(self.done()['kit'])
        touch(self.root, 'build/b')
        self.assertTrue(self.done()['kit'])

    def test_xenonrecomp_accepts_either_layout_but_needs_all_three_tools(self):
        m6 = ['build/cpu-xenonrecomp/XenonRecomp/XenonRecomp.exe', 'build/cpu-xenonrecomp/XenonAnalyse/XenonAnalyse.exe',
              'build/cpu-xex-decode.exe']
        for rel in m6[:2]:
            touch(self.root, rel)
        self.assertFalse(self.done()['xenonrecomp'])
        touch(self.root, m6[2])
        self.assertTrue(self.done()['xenonrecomp'])
        with tempfile.TemporaryDirectory() as other:
            for rel in ('build/catalog-tools/xenonrecomp-v23/XenonRecomp/XenonRecomp.exe',
                        'build/catalog-tools/xenonrecomp-v23/XenonAnalyse/XenonAnalyse.exe',
                        'build/prime-xex-decode-v18/rcomp_xex_decode.exe'):
                touch(other, rel)
            touch(other, 'deps/kit.lock.json', json.dumps({'assets': []}).encode())
            self.assertTrue({s.name: s.done() for s in sw.steps(other)}['xenonrecomp'])

    def test_a_folder_that_is_not_a_clone_fails_with_an_explanation(self):
        sources = sw.steps(self.root)[0]
        with mock.patch.object(sw.subprocess, 'call') as call, mock.patch('sys.stderr'):
            self.assertEqual(sources.run(), 1)
            call.assert_not_called()


class RunStepsTest(unittest.TestCase):
    def make(self, name, done, code=0, becomes_done=True):
        state = {'done': done, 'ran': 0}

        def run():
            state['ran'] += 1
            if becomes_done:
                state['done'] = True
            return code
        return sw.Step(name, name, lambda: state['done'], run), state

    def test_done_steps_are_skipped_and_the_rest_run_in_order(self):
        (a, sa), (b, sb), (c, sc) = self.make('a', True), self.make('b', False), self.make('c', False)
        lines = []
        self.assertEqual(sw.run_steps([a, b, c], log=lines.append), 0)
        self.assertEqual((sa['ran'], sb['ran'], sc['ran']), (0, 1, 1))
        self.assertTrue(lines[0].startswith('PASS     a: already done'))

    def test_the_first_failure_stops_everything(self):
        (a, sa), (b, sb), (c, sc) = self.make('a', False), self.make('b', False, code=3, becomes_done=False), self.make('c', False)
        lines = []
        self.assertEqual(sw.run_steps([a, b, c], log=lines.append), 1)
        self.assertEqual(sc['ran'], 0)
        self.assertTrue(any(l.startswith('FAIL     b: exit 3') for l in lines))

    def test_a_step_that_exits_zero_without_its_result_is_a_failure(self):
        a, _ = self.make('a', False, code=0, becomes_done=False)
        self.assertEqual(sw.run_steps([a], log=lambda text: None), 1)

    def test_a_missing_program_is_a_failure_not_a_traceback(self):
        def boom():
            raise FileNotFoundError('git')
        step = sw.Step('git-step', 'x', lambda: False, boom)
        lines = []
        with mock.patch('sys.stderr'):
            self.assertEqual(sw.run_steps([step], log=lines.append), 1)
        self.assertTrue(any('exit 127' in l for l in lines))

    def test_only_and_skip(self):
        (a, sa), (b, sb) = self.make('a', False), self.make('b', False)
        sw.run_steps([a, b], only=['b'], log=lambda text: None)
        self.assertEqual((sa['ran'], sb['ran']), (0, 1))
        (a, sa), (b, sb) = self.make('a', False), self.make('b', False)
        sw.run_steps([a, b], skip=['a'], log=lambda text: None)
        self.assertEqual((sa['ran'], sb['ran']), (0, 1))


class CygwinRunTest(unittest.TestCase):
    def test_windows_paths_become_cygdrive_paths(self):
        self.assertEqual(cygwin_run.to_cygwin_path('C:\\Users\\Me\\rcomp'), '/cygdrive/c/Users/Me/rcomp')
        self.assertEqual(cygwin_run.to_cygwin_path('D:/x/y'), '/cygdrive/d/x/y')
        self.assertEqual(cygwin_run.to_cygwin_path('/already/posix'), '/already/posix')

    def test_find_bash_prefers_the_bootstrapped_cygwin_and_honours_the_override(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertIsNone(cygwin_run.find_bash(root, environ={}))
            full = touch(root, 'build/vulkan-gta-radv-20260928/host-cygwin-full/bin/bash.exe')
            self.assertEqual(cygwin_run.find_bash(root, environ={}), full)
            boot = touch(root, 'build/prime-host-tools/cygwin/bin/bash.exe')
            self.assertEqual(cygwin_run.find_bash(root, environ={}), boot)
            other = touch(root, 'elsewhere/bash.exe')
            self.assertEqual(cygwin_run.find_bash(root, environ={'RCOMP_CYGWIN_BASH': str(other)}), other)
            self.assertIsNone(cygwin_run.find_bash(root, environ={'RCOMP_CYGWIN_BASH': str(root / 'missing.exe')}))

    def test_command_runs_the_words_from_the_checkout_with_the_pipeline_environment(self):
        cmd, env = cygwin_run.command(Path('C:/b/bash.exe'), ['bash', 'tools/m6_setup.sh'], 'C:\\work\\rcomp')
        self.assertEqual(cmd[-3:], ['_', 'bash', 'tools/m6_setup.sh'])
        self.assertIn('LLVM_CONFIG=/usr/bin/llvm-config', cmd[4])
        self.assertEqual(env['RCOMP_CWD'], '/cygdrive/c/work/rcomp')
        self.assertEqual(cygwin_run.command(Path('b'), [], '/x')[0][-2:], ['bash', '-i'])  # no words: an interactive shell

    def test_run_without_cygwin_reports_it(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ), mock.patch('sys.stderr'):
            os.environ.pop('RCOMP_CYGWIN_BASH', None)
            self.assertEqual(cygwin_run.run(['bash'], root=Path(tmp)), 127)


if __name__ == '__main__':
    unittest.main()
