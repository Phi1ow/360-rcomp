"""Real inventory plumbing with original XEX and external-tool TESTDOUBLEs."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import m6_inventory as inventory


def original_plain_xex():
    header, image = bytearray(0x400), bytearray(0x1000)
    header[:4] = b"XEX2"
    for offset, word in ((8, len(header)), (16, 0x100), (20, 2),
                         (24, 0x10100), (28, 0x82000200),
                         (32, 0x10201), (36, 0x82000000), (0x104, len(image))):
        struct.pack_into(">I", header, offset, word)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", image, 0x86, 1)
    struct.pack_into("<H", image, 0x94, 0xE0)
    image[0x178:0x180] = b".text\0\0\0"
    struct.pack_into("<II", image, 0x180, 4, 0x200)
    struct.pack_into("<I", image, 0x19C, 0x20)
    struct.pack_into(">I", image, 0x200, 0x4E800020)
    return bytes(header + image)


def original_execution_xex(offset=0x240):
    data = bytearray(original_plain_xex())
    struct.pack_into(">I", data, 20, 3)
    struct.pack_into(">II", data, 40, 0x40006, offset)
    # Original metadata, laid out according to Xenia's pinned
    # xex2_opt_execution_info: four BE32, four u8, then one BE32.
    # These distinct byte values catch field swaps and host-endian parsing.
    data[offset:offset + 24] = bytes.fromhex(
        "10203040 50607080 90A0B0C0 D0E0F001 11220304 12345678")
    return bytes(data)


class InventoryPipelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rcomp inventory spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_decoder_stdout_is_not_mixed_with_stderr(self):
        report, diagnostics = self.root / "decode.json", self.root / "decode.log"
        TESTDOUBLE_decoder = "import sys; print('{\"compression\":0}'); print('diagnostic after JSON',file=sys.stderr)"
        code, _ = inventory.run([sys.executable, "-c", TESTDOUBLE_decoder], report, stderr_log=diagnostics, timeout=10)
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(report.read_text()), {"compression": 0})
        self.assertIn("diagnostic after JSON", diagnostics.read_text())

    def test_real_tool_timeout_returns_failure_with_separate_diagnostics(self):
        TESTDOUBLE_slow_tool = "import time; time.sleep(1)"
        for separate in (False, True):
            with self.subTest(separate=separate):
                log = self.root / ("timeout-" + str(separate) + ".log")
                diagnostics = self.root / "timeout.stderr" if separate else None
                code, _ = inventory.run([sys.executable, "-c", TESTDOUBLE_slow_tool], log,
                                        timeout=0.1, stderr_log=diagnostics)
                self.assertEqual(code, 124)
                failure_log = diagnostics if separate else log
                self.assertIn("ERROR: tool timed out", failure_log.read_text())
                self.assertNotIn("Traceback", failure_log.read_text())
                if separate:
                    self.assertEqual(log.read_text(), "")

    def test_nonpositive_tool_timeout_rejected_before_running_tools(self):
        out = self.root / "invalid timeout"
        for value in ("0", "-1"):
            with self.subTest(timeout=value), \
                 mock.patch.object(sys, "argv", ["m6_inventory.py", "unused.xex", "--out", str(out), "--tool-timeout", value]), \
                 mock.patch.object(inventory, "run") as external_run, \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                inventory.main()
            self.assertEqual(error.exception.code, 2)
            external_run.assert_not_called()
            self.assertFalse(out.exists())

    def test_recompiler_timeout_keeps_report_and_blocks_gate(self):
        from m6_boot_gate import check_inventory
        image = self.root / "original.xex"; image.write_bytes(original_plain_xex())
        out = self.root / "timeout inventory"
        timeouts = []
        def TESTDOUBLE_external_run(command, log, cwd=None, timeout=None, stderr_log=None):
            timeouts.append(timeout)
            if stderr_log is not None:
                Path(command[2]).write_bytes(image.read_bytes())
                Path(log).write_text(json.dumps({"compression": 0, "encryption": 0}))
                Path(stderr_log).write_text("original fixture decoded\n")
            elif cwd is None:
                Path(command[2]).write_text("")
                Path(log).write_text("TESTDOUBLE XenonAnalyse: no tables\n")
            else:
                # A timed-out generator may already have apparently complete
                # output. Its nonzero exit must independently prevent linking.
                Path(cwd, "ppc/ppc_func_mapping.cpp").write_text(
                    "PPCFuncMapping PPCFuncMappings[] = {\n { 0x82000200, sub_entry },\n { 0, nullptr }\n};\n")
                Path(cwd, "ppc/ppc_recomp.0.cpp").write_text("void sub_entry() {}\n")
                Path(log).write_text("ERROR: tool timed out after 7 seconds\n")
                return 124, 7.0
            return 0, 0.0
        with mock.patch.object(sys, "argv", ["m6_inventory.py", str(image), "--out", str(out),
                                             "--require-supported", "--tool-timeout", "7"]), \
             mock.patch.object(inventory, "TOOLS", {key: sys.executable for key in ("decode", "analyse", "recomp")}), \
             mock.patch.object(inventory, "run", side_effect=TESTDOUBLE_external_run), \
             mock.patch.object(inventory, "export_tables", return_value={}), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(SystemExit) as error:
            inventory.main()
        self.assertEqual(error.exception.code, 1)
        self.assertEqual(timeouts, [7, 7, 7])
        report = json.loads((out / "inventory.json").read_text())
        self.assertEqual(report["xenonrecomp"]["exit_code"], 124)
        self.assertEqual(report["xenonrecomp"]["functions"], 1)
        self.assertFalse(report["supported_for_link"])
        self.assertIn("XenonRecomp exited 124", report["blocking_reasons"])
        self.assertTrue(check_inventory(report, out))

    def test_unclassified_recompiler_errors_block_linking(self):
        warnings, _ = inventory.classify_log("ERROR: Unable to load the patch file\n")
        self.assertTrue(warnings)

    def test_missing_helpers_are_only_advisory_when_absent(self):
        text = "ERROR: __savegprlr_14 address is unspecified\n"
        warnings, _ = inventory.classify_log(text)
        self.assertTrue(warnings)
        warnings, _ = inventory.classify_log(text, {"__savegprlr_14"})
        self.assertEqual(warnings, {})

    def test_existing_run_cannot_supply_stale_artifacts(self):
        out = self.root / "existing"; out.mkdir()
        sentinel = out / "plain.xex"; sentinel.write_bytes(b"stale image")
        with self.assertRaisesRegex(ValueError, "already exists"): inventory.prepare_output(out)
        self.assertEqual(sentinel.read_bytes(), b"stale image")

    def test_fresh_run_and_git_output_restriction(self):
        (self.root / ".git").mkdir()
        with self.assertRaises(ValueError): inventory.prepare_output(self.root / "derived")
        out = Path(inventory.prepare_output(self.root / "build" / "new run"))
        self.assertTrue((out / "ppc").is_dir())

    def test_symlink_cannot_authorize_output(self):
        (self.root / ".git").mkdir()
        target = self.root / "sources"; target.mkdir()
        try: (self.root / "build").symlink_to(target, target_is_directory=True)
        except OSError as error: self.skipTest("host does not permit symlinks: " + str(error))
        self.assertFalse(inventory.output_allowed(self.root / "build" / "derived"))

    def run_original_inventory(self, plain, source=None):
        out = self.root / "new inventory"
        image = self.root / "original.xex"; image.write_bytes(plain if source is None else source)
        def TESTDOUBLE_external_run(command, log, cwd=None, timeout=None, stderr_log=None):
            if stderr_log is not None:
                Path(command[2]).write_bytes(plain)
                Path(log).write_text(json.dumps({"compression": 0, "encryption": 0}))
                Path(stderr_log).write_text("original fixture decoded\n")
            else:
                Path(command[2]).write_text("")
                Path(log).write_text("TESTDOUBLE XenonAnalyse: no tables\n")
            return 0, 0.0
        with mock.patch.object(sys, "argv", ["m6_inventory.py", str(image), "--out", str(out)]), \
             mock.patch.object(inventory, "TOOLS", {key: sys.executable for key in ("decode", "analyse", "recomp")}), \
             mock.patch.object(inventory, "run", side_effect=TESTDOUBLE_external_run), \
             mock.patch.object(inventory, "export_tables", return_value={}), contextlib.redirect_stdout(io.StringIO()):
            inventory.main()
        report = json.loads((out / "inventory.json").read_text())
        return report, out

    def test_inventory_only_report_explicitly_forbids_link(self):
        report, _ = self.run_original_inventory(original_plain_xex())
        self.assertFalse(report["supported_for_link"])
        self.assertFalse(report["xenonrecomp"]["ran"])
        self.assertIn("recompilation was not run; inventory only", report["blocking_reasons"])
        self.assertEqual(report["image"]["entry"], "0x82000200")

    def test_execution_info_absence_is_explicit(self):
        info, _, _ = inventory.parse_plain_xex(original_plain_xex())
        self.assertIn("execution_info", info)
        self.assertIsNone(info["execution_info"])

    def test_execution_info_fields_and_exact_header_boundary(self):
        for offset in (0x240, 0x3E8):
            with self.subTest(offset=offset):
                info, _, _ = inventory.parse_plain_xex(original_execution_xex(offset))
                self.assertEqual(info["execution_info"], {
                    "media_id": "0x10203040", "version": "0x50607080",
                    "base_version": "0x90A0B0C0", "title_id": "0xD0E0F001",
                    "platform": 0x11, "executable_table": 0x22,
                    "disc_number": 3, "disc_count": 4, "savegame_id": "0x12345678",
                })

    def test_execution_info_duplicate_rejected(self):
        data = bytearray(original_execution_xex())
        struct.pack_into(">I", data, 20, 4)
        struct.pack_into(">II", data, 48, 0x40006, 0x240)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            inventory.parse_plain_xex(bytes(data))

    def test_execution_info_truncated_at_header_rejected(self):
        # Plenty of image bytes follow the header, but they cannot complete it.
        data = bytearray(original_execution_xex())
        for offset in (0x3E9, 0x400):
            with self.subTest(offset=offset):
                struct.pack_into(">I", data, 44, offset)
                with self.assertRaisesRegex(ValueError, "truncated"):
                    inventory.parse_plain_xex(bytes(data))

    def test_execution_info_optional_table_overlap_rejected(self):
        data = bytearray(original_execution_xex())
        for offset in (0, 24, 47):
            with self.subTest(offset=offset):
                struct.pack_into(">I", data, 44, offset)
                with self.assertRaises(ValueError):
                    inventory.parse_plain_xex(bytes(data))

    def test_report_retains_execution_identity_and_original_source_hash(self):
        plain = original_execution_xex()
        # A decoder may change the container; fingerprint the source, not plain.xex.
        source = plain + b"original synthetic container trailer"
        report, out = self.run_original_inventory(plain, source)
        self.assertEqual(report["image"]["execution_info"]["title_id"], "0xD0E0F001")
        self.assertEqual(report["image"]["execution_info"]["disc_number"], 3)
        self.assertEqual(report["image"]["execution_info"]["disc_count"], 4)
        self.assertEqual(report["source_xex"], {
            "size": len(source), "sha256": hashlib.sha256(source).hexdigest(),
        })
        self.assertNotEqual(report["source_xex"]["sha256"], hashlib.sha256(plain).hexdigest())
        self.assertEqual((out / "plain.xex").read_bytes(), plain)

    def test_static_tls_header_is_parsed_in_documented_order(self):
        data = bytearray(original_plain_xex())
        struct.pack_into(">I", data, 20, 3)
        struct.pack_into(">II", data, 40, 0x20104, 0x240)
        struct.pack_into(">4I", data, 0x240, 2, 0x82000200, 16, 4)
        info, _, _ = inventory.parse_plain_xex(bytes(data))
        self.assertEqual(info["static_tls"], {"slot_count": 2, "raw_data_address": 0x82000200, "data_size": 16, "raw_data_size": 4})

    def test_static_tls_duplicate_or_truncated_header_rejected(self):
        data = bytearray(original_plain_xex()); struct.pack_into(">I", data, 20, 3)
        struct.pack_into(">II", data, 40, 0x20104, 0x3F8)
        with self.assertRaisesRegex(ValueError, "truncated"): inventory.parse_plain_xex(bytes(data))
        struct.pack_into(">I", data, 20, 4)
        struct.pack_into(">II", data, 40, 0x20104, 0x240)
        struct.pack_into(">II", data, 48, 0x20104, 0x250)
        with self.assertRaisesRegex(ValueError, "duplicate"): inventory.parse_plain_xex(bytes(data))


if __name__ == "__main__": unittest.main()
