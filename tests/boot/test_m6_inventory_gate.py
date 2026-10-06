"""Synthetic metadata/files only. These tests do NOT run XEX code or a GPU."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from m6_boot_gate import artifact_snapshot, check_inventory, static_tls_blockers


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        (self.work / "ppc").mkdir()
        for name, text in {"plain.xex": "synthetic, not a real XEX", "game.toml": "[main]\n",
                           "switch_tables.toml": "", "ppc/ppc_recomp.0.cpp": "void sub_entry() {}\n", "ppc/ppc_func_mapping.cpp":
                           "PPCFuncMapping PPCFuncMappings[] = {\n { 0x82000000, sub_entry },\n { 0, nullptr }\n};\n"}.items():
            (self.work / name).write_text(text)
        self.report = {"supported_for_link": True, "blocking_reasons": [],
                       "decode": {"encryption": 0, "compression": 0, "supported": True},
                       "jump_tables": {"xenonanalyse_exit": 0, "tables": 0},
                       "xenonrecomp": {"ran": True, "exit_code": 0, "functions": 1,
                                       "generated_bytes": 100, "warnings": {}, "unrecognized_instructions": {}},
                       "imports": {"xboxkrnl.exe": {"functions_missing": [], "variables_missing": [], "unknown_ordinals": []}},
                       "image": {"entry": "0x82000000", "optional_headers": []}}
        self.report["artifacts"] = artifact_snapshot(self.work)
        self.report["xenonrecomp"]["generated_bytes"] = sum(
            entry["bytes"] for name, entry in self.report["artifacts"].items() if name.startswith("ppc/"))

    def test_changed_generated_source_is_not_silently_reused(self):
        (self.work / "ppc/ppc_recomp.0.cpp").write_text("void stale_entry() {}\n")
        self.assertTrue(any("artifact hashes" in error for error in check_inventory(self.report, self.work)))

    def test_mapping_alone_is_not_generated_code(self):
        (self.work / "ppc/ppc_recomp.0.cpp").unlink()
        self.report["artifacts"] = artifact_snapshot(self.work)
        self.report["xenonrecomp"]["generated_bytes"] = sum(
            entry["bytes"] for name, entry in self.report["artifacts"].items() if name.startswith("ppc/"))
        self.assertTrue(any("implementation source" in error for error in check_inventory(self.report, self.work)))

    def test_extra_stale_source_is_rejected(self):
        (self.work / "ppc/obsolete.cpp").write_text("void obsolete() {}\n")
        self.assertTrue(check_inventory(self.report, self.work))

    def test_static_tls_verified_subset_is_accepted(self):
        self.report["image"].update({"base": "0x82000000", "size": 0x1000, "optional_headers": ["0x00020104"]})
        for slots, data_size, raw_size, address in ((2,16,4,0x82000080),(0,0,0,0),(2,0,0,0),(2,16,0,0xFFFFFFFF)):
            with self.subTest(slots=slots, raw_size=raw_size):
                self.report["image"]["static_tls"] = {"slot_count": slots, "data_size": data_size,
                    "raw_data_size": raw_size, "raw_data_address": address}
                self.assertEqual(check_inventory(self.report, self.work), [])

    def test_static_tls_invalid_metadata_is_not_accepted(self):
        image = {"base": "0x82000000", "size": 0x1000, "optional_headers": ["0x00020104"],
            "static_tls": {"slot_count": 2, "data_size": 16, "raw_data_size": 4, "raw_data_address": 0x82000080}}
        for key, value in (("raw_data_size",17),("slot_count",0),("slot_count",4194304),("slot_count",True),
                ("data_size",-1),("raw_data_address",0),("raw_data_address",0x82000FFE),("raw_data_address",0xFFFFFFFF)):
            with self.subTest(key=key, value=value):
                changed = copy.deepcopy(image); changed["static_tls"][key] = value
                self.assertTrue(static_tls_blockers(changed))
        image["optional_headers"].append("0x00020104")
        self.assertTrue(static_tls_blockers(image))

    def test_static_tls_allocation_limit_counts_slots_and_template(self):
        image = {"optional_headers": ["0x00020104"], "static_tls": {"slot_count": 1,
            "data_size": 16 * 1024 * 1024 - 4, "raw_data_size": 0, "raw_data_address": 0}}
        self.assertEqual(static_tls_blockers(image), [])
        image["static_tls"]["data_size"] += 1
        self.assertTrue(static_tls_blockers(image))

    def test_complete_synthetic_inventory(self):
        self.assertEqual(check_inventory(self.report, self.work), [])

    def test_inventory_only_never_linkable(self):
        self.report["xenonrecomp"] = {"ran": False}
        self.assertTrue(check_inventory(self.report, self.work))

    def test_missing_mapping_and_stale_count(self):
        (self.work / "ppc/ppc_func_mapping.cpp").unlink()
        self.assertTrue(check_inventory(self.report, self.work))

    def test_wrong_entry_and_duplicate_addresses(self):
        for text in ("{ 0x82000004, wrong }", "{ 0x82000000, a }, { 0x82000000, b }"):
            with self.subTest(text=text):
                (self.work / "ppc/ppc_func_mapping.cpp").write_text(text)
                self.assertTrue(check_inventory(self.report, self.work))

    def test_static_tls_is_not_dynamic_tls(self):
        self.report["image"]["optional_headers"] = ["0x00020104"]
        self.assertTrue(any("static TLS" in e for e in check_inventory(self.report, self.work)))

    def test_fail_closed_each_missing_import_kind(self):
        for field in ("functions_missing", "variables_missing", "unknown_ordinals"):
            with self.subTest(field=field):
                r = copy.deepcopy(self.report)
                r["imports"]["xboxkrnl.exe"][field] = ["synthetic_missing"]
                self.assertTrue(check_inventory(r, self.work))

    def test_recompile_errors_cannot_be_overridden_by_true_flag(self):
        for key, value in (("exit_code", 1), ("functions", None), ("functions", True),
                           ("generated_bytes", 0), ("warnings", {"unknown": 1}),
                           ("unrecognized_instructions", {"fake_instruction": 1})):
            with self.subTest(key=key):
                r = copy.deepcopy(self.report)
                r["xenonrecomp"][key] = value
                self.assertTrue(check_inventory(r, self.work))

    def test_missing_sections_and_decoder_shape(self):
        for key in ("imports", "image", "decode", "xenonrecomp", "jump_tables"):
            with self.subTest(key=key):
                r = copy.deepcopy(self.report)
                del r[key]
                self.assertTrue(check_inventory(r, self.work))
        self.report["decode"]["compression"] = True
        self.assertTrue(check_inventory(self.report, self.work))

    def test_cli_produces_non_boot_claim_and_input_hash(self):
        (self.work / "inventory.json").write_text(json.dumps(self.report))
        proc = subprocess.run([sys.executable, str(ROOT / "tools/m6_boot_gate.py"), str(self.work),
                               "--xex", str(self.work / "plain.xex")], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        gate = json.loads((self.work / "BOOT_GATE.json").read_text())
        self.assertEqual(gate["gta_iv_boot"], "NOT TESTED")
        self.assertEqual(gate["status"], "PASS")
        self.assertEqual(gate["input_xex_sha256"], hashlib.sha256((self.work / "plain.xex").read_bytes()).hexdigest())

    def test_gate_output_symlink_never_overwrites_sentinel(self):
        sentinel = self.work / "sentinel.txt"
        sentinel.write_text("preserve")
        try: (self.work / "BOOT_GATE.json").symlink_to(sentinel)
        except OSError as error: self.skipTest("host does not permit symlinks: " + str(error))
        (self.work / "inventory.json").write_text(json.dumps(self.report))
        result = subprocess.run([sys.executable, str(ROOT / "tools/m6_boot_gate.py"), str(self.work)],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
