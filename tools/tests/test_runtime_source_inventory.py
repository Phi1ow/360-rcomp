"""Original source snapshots check build/registration selection, not game data."""
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from runtime_source_inventory import source_imports


class SourceSelectionTests(unittest.TestCase):
    def setUp(self):
        output = TOOLS.parent / 'build/prime-runtime-source-tests'
        output.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=output)
        self.root = Path(self.temporary.name)
        (self.root / 'runtime/src').mkdir(parents=True)
        (self.root / 'app/src').mkdir(parents=True)
        self.write('runtime/CMakeLists.txt', '''
add_library(rcomp_runtime STATIC src/hle_xboxkrnl.cpp src/hle_xam.cpp src/thread_object.cpp)
add_library(rcomp_runtime_video STATIC src/hle_xboxkrnl_video.cpp)
''')
        self.write('runtime/src/hle_xboxkrnl.cpp', '''
const Impl entries[] = {{0x1, "KernelReal", &KernelReal}};
Status register_xboxkrnl_hle() { return Status::Ok; }
''')
        self.write('runtime/src/hle_xam.cpp', '''
const Impl entries[] = {{0x2, "XamReal", &XamReal}};
Status register_xam_hle() { return Status::Ok; }
''')
        self.write('runtime/src/hle_xboxkrnl_video.cpp', '''
struct Variable {} vars[] = {{0x3, address, "VideoReal"}};
Status register_xboxkrnl_video_hle() { return Status::Ok; }
''')
        self.write('runtime/src/thread_object.cpp', '''
Status register_thread_object_type_variable() {
  return register_variable_import(kModuleXboxkrnl, 0x001B, address, "ExThreadObjectType");
}
''')
        self.write('app/src/title_runtime.cpp', 'void create() { rt::register_thread_object_type_variable(); }')

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, name, text):
        (self.root / name).write_text(text, encoding='utf-8')

    def test_unintegrated_file_not_supported(self):
        self.write('runtime/src/hle_xam_net.cpp', '''
const Impl entries[] = {{0x99, "Unintegrated", &Unintegrated}};
Status register_xam_net_hle() { return Status::Ok; }
''')
        functions, variables, _ = source_imports(self.root)
        self.assertEqual(functions, {('xboxkrnl.exe', 1), ('xam.xex', 2)})
        self.assertEqual(variables, {('xboxkrnl.exe', 3), ('xboxkrnl.exe', 0x1B)})

    def test_linked_but_unregistered_file_not_supported(self):
        self.test_unintegrated_file_not_supported()
        path = self.root / 'runtime/CMakeLists.txt'
        path.write_text(path.read_text().replace('src/hle_xam.cpp', 'src/hle_xam.cpp src/hle_xam_net.cpp'))
        self.assertNotIn(('xam.xex', 0x99), source_imports(self.root)[0])
        path = self.root / 'runtime/src/hle_xam.cpp'
        path.write_text(path.read_text().replace('return Status::Ok;', 'return register_xam_net_hle();'))
        self.assertIn(('xam.xex', 0x99), source_imports(self.root)[0])

    def test_missing_registration_source_fails(self):
        path = self.root / 'runtime/src/hle_xam.cpp'
        path.write_text(path.read_text().replace('return Status::Ok;', 'return register_xam_absent_hle();'))
        with self.assertRaisesRegex(ValueError, 'missing from runtime build'):
            source_imports(self.root)

    def test_comments_do_not_add_support(self):
        path = self.root / 'runtime/src/hle_xam.cpp'
        path.write_text(path.read_text() + '\n// {0x99,"Comment",&Comment}\n/* {0x98,"Comment",&Comment} */\n')
        self.assertEqual(source_imports(self.root)[0], {('xboxkrnl.exe', 1), ('xam.xex', 2)})

    def test_kernel_variables_need_both_build_and_bootstrap(self):
        self.write('runtime/src/kernel_variables.cpp', '''
Status register_xboxkrnl_kernel_variables_from_xex(const uint8_t* data, size_t size) {
  register_variable_import(kModuleXboxkrnl, 0x0158, 0x70000100, "XboxKrnlVersion");
  return register_variable_import(kModuleXboxkrnl, 0x00AD, 0x70000200, "KeTimeStampBundle");
}
''')
        path = self.root / 'runtime/CMakeLists.txt'
        path.write_text(path.read_text().replace('src/thread_object.cpp',
                        'src/thread_object.cpp src/kernel_variables.cpp'))
        self.assertNotIn(('xboxkrnl.exe', 0x158), source_imports(self.root)[1])
        path = self.root / 'app/src/title_runtime.cpp'
        path.write_text(path.read_text() + '\nvoid configure() { rt::register_xboxkrnl_kernel_variables_from_xex(x, n); }')
        self.assertIn(('xboxkrnl.exe', 0x158), source_imports(self.root)[1])
        self.assertIn(('xboxkrnl.exe', 0xAD), source_imports(self.root)[1])


if __name__ == '__main__':
    unittest.main()
