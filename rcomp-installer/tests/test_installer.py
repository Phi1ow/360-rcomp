"""Offline tests (no console, no game content): python -m unittest discover -s tests"""
import json
import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from rinstaller import package, pipeline, png, profiles, xex  # noqa: E402
from rinstaller import settings as settings_mod  # noqa: E402
from rinstaller.jobs import Blocked, Failed  # noqa: E402


def make_xdbf(names, default_lang, image):
    """XDBF with an XSTC (default language), one XSTR per language and an optional title image."""
    blobs = [(1, 0x58535443, b'XSTC' + struct.pack('>III', 1, 8, default_lang))]
    for lang, text in names.items():
        s = text.encode()
        blobs.append((3, lang, b'XSTR' + struct.pack('>IIH', 1, 0, 1) + struct.pack('>HH', 0x8000, len(s)) + s))
    if image:
        blobs.append((2, 0x8000, image))
    entries, data = b'', b''
    for ns, ident, blob in blobs:
        entries += struct.pack('>HQII', ns, ident, len(data), len(blob))
        data += blob
    return b'XDBF' + struct.pack('>IIIII', 0x10000, len(blobs), len(blobs), 0, 0) + entries + data


def make_xex(title_id=0x41570001, names=None, default_lang=1, image=b'', plain=True):
    """A synthetic plain XEX2 header with execution info, image base, file format and an XDBF resource."""
    base, pe_off = 0x82000000, 0x1000
    xdbf = make_xdbf(names or {1: 'Test Title'}, default_lang, image)
    headers = [(0x00040006, 0x200), (0x00010201, base), (0x000003FF, 0x240), (0x000002FF, 0x260)]
    b = bytearray(pe_off + 0x100 + len(xdbf))
    b[0:4] = b'XEX2'
    struct.pack_into('>I', b, 8, pe_off)
    struct.pack_into('>I', b, 20, len(headers))
    for i, (k, v) in enumerate(headers):
        struct.pack_into('>II', b, 24 + 8 * i, k, v)
    struct.pack_into('>IIIIBBBB', b, 0x200, 0x1234ABCD, 7, 7, title_id, 0, 0, 2, 3)
    struct.pack_into('>IHH', b, 0x240, 8, 0 if plain else 1, 0)
    name = ('%08X' % title_id).encode()
    struct.pack_into('>I8sII', b, 0x260, 4 + 16, name, base + 0x100, len(xdbf))
    b[pe_off + 0x100:] = xdbf
    return bytes(b)


class Log:
    def __init__(self):
        self.lines = []

    def __call__(self, level, text):
        self.lines.append((level, text))


class FakeJob:
    def __init__(self):
        self.log = Log()

    def check_cancel(self):
        pass


class XexTests(unittest.TestCase):
    def test_identity_and_names(self):
        icon = png.text_tile('X', side=64)
        info = xex.read_xex(make_xex(names={1: 'English Name', 4: 'Nom'}, default_lang=4, image=icon))
        self.assertEqual(info['title_id'], '41570001')
        self.assertEqual(info['media_id'], '1234ABCD')
        self.assertEqual((info['disc_number'], info['disc_count']), (2, 3))
        self.assertTrue(info['plain'])
        self.assertEqual(info['name'], 'Nom')  # default language first
        self.assertEqual(info['names']['English'], 'English Name')
        self.assertEqual(info['icon_png'], icon)

    def test_english_fallback(self):
        info = xex.read_xex(make_xex(names={1: 'Only English'}, default_lang=3))
        self.assertEqual(info['name'], 'Only English')

    def test_not_plain(self):
        info = xex.read_xex(make_xex(plain=False))
        self.assertFalse(info['plain'])
        self.assertEqual(info['name'], '')
        self.assertTrue(info['warnings'])

    def test_garbage(self):
        with self.assertRaises(xex.XexError):
            xex.read_xex(b'NOPE' + bytes(64))
        with self.assertRaises(xex.XexError):
            xex.read_xex(b'XEX2' + bytes(8))


class PngTests(unittest.TestCase):
    def test_roundtrip_and_fit(self):
        tile = png.text_tile('Hello World', side=128)
        w, h, rgba = png.decode(tile)
        self.assertEqual((w, h, len(rgba)), (128, 128, 128 * 128 * 4))
        self.assertEqual(png.size(png.fit_icon(tile)), (512, 512))


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / 'in/game/sub').mkdir(parents=True)
        (self.root / 'in/eboot.bin').write_bytes(b'\x7fELF' + bytes(60))
        (self.root / 'in/libc.prx').write_bytes(b'prx' * 10)
        (self.root / 'in/plain.xex').write_bytes(make_xex(names={1: 'Sample'}))
        (self.root / 'in/game/a.bin').write_bytes(b'a' * 100)
        (self.root / 'in/game/sub/b.bin').write_bytes(b'b' * 50)

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, **kw):
        i = self.root / 'in'
        args = dict(eboot=i / 'eboot.bin', xex_path=i / 'plain.xex', game=i / 'game', libc=i / 'libc.prx',
                    out_parent=self.root / 'out', title_id='PPSA88370', name=None)
        args.update(kw)
        return package.build_app(FakeJob(), **args)

    def test_build_and_inspect(self):
        res = self.build()
        app = pathlib.Path(res['app'])
        self.assertEqual(app.name, 'PPSA88370')
        param = json.loads((app / 'sce_sys/param.json').read_text())
        self.assertEqual(package.check_param(param), [])
        self.assertEqual(param['contentId'], 'UP9000-PPSA88370_00-RCOMP88370000000')
        self.assertEqual(param['localizedParameters']['en-US']['titleName'], 'Sample')
        self.assertEqual(png.size((app / 'sce_sys/icon0.png').read_bytes()), (512, 512))
        manifest = (app / 'manifest.sha256').read_text()
        self.assertIn('  ./game/sub/b.bin\n', manifest)
        info = package.inspect_app(app, Log(), lambda: None)
        self.assertEqual(info['title_id'], 'PPSA88370')
        self.assertNotIn(package.MARKER, [r for r, *_ in info['files']])
        (app / 'game/a.bin').write_bytes(b'tampered')
        with self.assertRaises(Failed):
            package.inspect_app(app, Log(), lambda: None)

    def test_refusals(self):
        with self.assertRaises(Blocked):
            self.build(title_id='CUSA12345')
        self.build()
        with self.assertRaises(Blocked):
            self.build()  # exists
        self.build(replace=True, name='Renamed')
        with self.assertRaises(Blocked):
            self.build(out_parent=self.root / 'in' / 'game')  # overlaps the game folder
        with self.assertRaises(Blocked):
            self.build(forbidden_roots=[self.root])

    def test_check_param(self):
        p = package.make_param('PPSA88370', 'X')
        p['conceptId'] = '00000'
        self.assertTrue(package.check_param(p))


class ProfileTests(unittest.TestCase):
    def test_detect(self):
        self.assertEqual(profiles.detect('545407F2', '4A53F9F6')[0], 'gta4')
        self.assertEqual(profiles.detect('545407F2', '06759F9C')[0], 'eflc')
        self.assertEqual(profiles.detect('41570001', '00000000')[0], 'generic')
        with tempfile.TemporaryDirectory() as d:
            (pathlib.Path(d) / 'DLC2').mkdir()
            self.assertEqual(profiles.detect('545407F2', '12345678', pathlib.Path(d))[0], 'eflc')
            self.assertEqual(profiles.detect('545407F2', '12345678', pathlib.Path(d) / 'none')[0], 'gta4')

    def test_player_defaults_everywhere(self):
        for key, p in profiles.PROFILES.items():
            self.assertEqual(p['options']['RCOMP_M6_PC_SAMPLER'], 'OFF', key)
            self.assertEqual(p['options']['RCOMP_M6_CAPTURE_INTERVAL_SECONDS'], '0', key)
            self.assertEqual(p['options']['RCOMP_PS5_AUTOPILOT'], 'OFF', key)
            self.assertEqual(p['options']['RCOMP_M6_LAUNCH_MENU'], 'ON', key)

    def test_tuned_profiles_carry_the_measured_generator_options(self):
        want = {'cr_as_local': 'true', 'ctr_as_local': 'true', 'xer_as_local': 'true', 'reserved_as_local': 'true'}
        self.assertEqual(profiles.PROFILES['gta4']['generator'], want)
        self.assertEqual(profiles.PROFILES['eflc']['generator'], want)
        self.assertEqual(profiles.PROFILES['generic']['generator'], {})
        for key, p in profiles.PROFILES.items():  # group 2 is rejected: functions read those registers as inputs
            self.assertNotIn('non_argument_as_local', p['generator'], key)
            self.assertNotIn('non_volatile_as_local', p['generator'], key)

    def test_max_test_profiles(self):
        for key in ('gta4max', 'gta4maxcap', 'eflcmax'):
            p = profiles.PROFILES[key]
            self.assertEqual(p['generator'], profiles.RAGE_GENERATOR, key)
            self.assertIn('-fomit-frame-pointer', p['options']['RCOMP_M6_GENERATED_OPT'], key)
            self.assertTrue(p['options']['RCOMP_M6_GENERATED_OPT'].startswith('-O3'), key)
            self.assertEqual(p['options']['RCOMP_M6_LAUNCH_MENU'], 'ON', key)
            self.assertTrue(p['link_order'].endswith('_hot_order.txt'), key)
        env = profiles.PROFILES['gta4max']['options']['RCOMP_M6_TUNING_ENV']
        capped = profiles.PROFILES['gta4maxcap']['options']['RCOMP_M6_TUNING_ENV']
        self.assertNotIn('RCOMP_SLEEP_CAP_NS', env)
        self.assertEqual(capped, env + ',RCOMP_SLEEP_CAP_NS=250000')
        # EFLC keeps its own thread placement (the arm-197 placement of GTA IV is not applied to it)
        self.assertNotIn('RCOMP_CPU_MASK_GUEST_MAIN', profiles.PROFILES['eflcmax']['options']['RCOMP_M6_TUNING_ENV'])

    def test_link_order_file(self):
        self.assertIsNone(pipeline.link_order_file({}))
        self.assertIsNone(pipeline.link_order_file({'link_order': 'does_not_exist.txt'}))

    def test_recomp_option_args(self):
        self.assertEqual(pipeline.recomp_option_args(None), [])
        self.assertEqual(pipeline.recomp_option_args({}), [])
        self.assertEqual(pipeline.recomp_option_args({'xer_as_local': 'true', 'cr_as_local': 'true'}),
                         ['--recomp-option', 'cr_as_local=true', '--recomp-option', 'xer_as_local=true'])

    def test_choose_profile(self):
        info = {'title_id': '545407F2', 'media_id': '4A53F9F6'}
        self.assertEqual(pipeline.choose_profile({}, info, None)[0], 'gta4')
        self.assertEqual(pipeline.choose_profile({'profile': 'auto'}, info, None)[0], 'gta4')
        self.assertEqual(pipeline.choose_profile({'profile': 'eflc'}, info, None)[::2], ('eflc', profiles.PROFILES['eflc']))
        with self.assertRaises(pipeline.Blocked):
            pipeline.choose_profile({'profile': 'nope'}, info, None)


class FakeSettings:
    def __init__(self, root):
        self.values = {'rcomp_root': str(root), 'cygwin_bash': ''}

    def get(self, key):
        return self.values.get(key, '')


def touch(root, rel):
    p = pathlib.Path(root, rel)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b'x')
    return p


class ToolchainLayoutTests(unittest.TestCase):
    def test_first_existing(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = pathlib.Path(tmp, 'a'), pathlib.Path(tmp, 'b')
            self.assertEqual(pipeline.first_existing(a, b), b)  # none exists: the last candidate is reported
            a.write_bytes(b'x')
            self.assertEqual(pipeline.first_existing(a, b), a)
            b.write_bytes(b'y')
            self.assertEqual(pipeline.first_existing(a, b), a)  # both exist: the first wins

    def test_a_fresh_checkout_layout_is_found_and_the_developer_layout_wins(self):
        with tempfile.TemporaryDirectory() as tmp:
            fresh = {'decode': 'build/cpu-xex-decode.exe', 'analyse': 'build/cpu-xenonrecomp/XenonAnalyse/XenonAnalyse.exe',
                     'recomp': 'build/cpu-xenonrecomp/XenonRecomp/XenonRecomp.exe'}
            for rel in fresh.values():  # what tools/m6_setup.sh builds
                touch(tmp, rel)
            pathlib.Path(tmp, 'build/cpu-xenonrecomp-src/XenonUtils').mkdir(parents=True)
            p = pipeline.paths(FakeSettings(tmp))
            for key, rel in fresh.items():
                self.assertEqual(p[key], pathlib.Path(tmp, rel), key)
            self.assertEqual(p['xenon_src'], pathlib.Path(tmp, 'build/cpu-xenonrecomp-src'))
            ready = pipeline.tool_report(FakeSettings(tmp))['tools']
            for key in ('xex_decode', 'xenon_analyse', 'xenon_recomp', 'xenon_source'):
                self.assertEqual(ready[key]['status'], 'PASS', key)
            touch(tmp, 'build/catalog-tools/xenonrecomp-v23/XenonRecomp/XenonRecomp.exe')
            self.assertEqual(pipeline.paths(FakeSettings(tmp))['recomp'],
                             pathlib.Path(tmp, 'build/catalog-tools/xenonrecomp-v23/XenonRecomp/XenonRecomp.exe'))

    def test_a_missing_tool_names_the_command_that_provides_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools = pipeline.tool_report(FakeSettings(tmp))['tools']
        self.assertEqual(set(pipeline.HINTS), set(tools))
        for key in ('extract_xiso', 'xex_decode', 'xenon_analyse', 'xenon_recomp', 'xenon_source'):
            self.assertIn('setup_windows', tools[key]['hint'], key)
        for key in ('xenos_source', 'ps5_sdk', 'radv_driver', 'xma_codec', 'ps5_native_tool', 'libc_prx'):
            self.assertIn('install_kit', tools[key]['hint'], key)
        self.assertIn('bootstrap_cygwin', tools['cygwin_bash']['hint'])
        with tempfile.TemporaryDirectory() as tmp:
            touch(tmp, 'tools/m6_inventory.py')
            self.assertNotIn('hint', pipeline.tool_report(FakeSettings(tmp))['tools']['m6_inventory'])  # present: no hint


class SettingsLayoutTests(unittest.TestCase):
    def test_installer_inside_the_checkout_uses_the_checkout_and_a_folder_beside_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            tool = pathlib.Path(tmp, 'checkout', 'rcomp-installer')
            tool.mkdir(parents=True)
            touch(tmp, 'checkout/tools/m6_inventory.py')
            root, data = settings_mod.default_layout(tool, pathlib.Path(tmp, 'home'))
            self.assertEqual(root, pathlib.Path(tmp, 'checkout'))
            self.assertEqual(data, pathlib.Path(tmp, 'rcomp-installer-data'))
            self.assertNotIn(root, data.parents)  # the pipeline refuses a work folder inside the checkout

    def test_a_separate_copy_of_the_tool_keeps_the_earlier_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            tool = pathlib.Path(tmp, 'installer')
            tool.mkdir()
            root, data = settings_mod.default_layout(tool, pathlib.Path(tmp, 'home'))
            self.assertEqual(root, pathlib.Path(tmp, 'home', 'rcomp'))
            self.assertEqual(data, tool / 'build')

    def test_default_work_and_output_folders_are_never_inside_the_checkout(self):
        d = settings_mod.DEFAULTS
        root = pathlib.Path(d['rcomp_root'])
        for key in ('work_dir', 'output_dir'):
            folder = pathlib.Path(d[key])
            self.assertNotIn(root, [folder, *folder.parents], key)


if __name__ == '__main__':
    unittest.main()
