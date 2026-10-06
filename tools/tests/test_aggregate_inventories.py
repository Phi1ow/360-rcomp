"""Synthetic catalogs (no game data) for the disc extraction and aggregation tools."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import aggregate_inventories as agg  # noqa: E402
import iso_extract  # noqa: E402


def xex_header(title_id=0x41410001, media_id=0x12345678, disc=1, discs=1, flags=0x1, libs=("xboxkrnl.exe", "xam.xex")):
    """A minimal plaintext XEX2 header: execution info, original name, import libraries."""
    count = 3
    table = 24 + 8 * count
    exec_off = table
    exec_info = struct.pack(">4I4BI", media_id, 0x6, 0x6, title_id, 0, 0, disc, discs, 0)
    name_off = exec_off + len(exec_info)
    name = b"default.exe\0"
    name_blob = struct.pack(">I", 4 + len(name)) + name
    name_blob += b"\0" * (-len(name_blob) % 4)
    imp_off = name_off + len(name_blob)
    strings = b""
    for lib in libs:
        s = lib.encode() + b"\0"
        strings += s + b"\0" * (-len(s) % 4)
    records = b""
    for i, _ in enumerate(libs):
        records += struct.pack(">I20sIII2H", 40, b"\0" * 20, 0, 0x201DDD00 + i, 0x20176000, i, 0)
    imp = struct.pack(">3I", 12 + len(strings) + len(records), len(strings), len(libs)) + strings + records
    security = imp_off + len(imp)
    header_size = security + 0x180
    head = struct.pack(">4s5I", b"XEX2", flags, header_size, 0, security, count)
    head += struct.pack(">2I", 0x40006, exec_off) + struct.pack(">2I", 0x183FF, name_off) + struct.pack(">2I", 0x103FF, imp_off)
    body = head + exec_info + name_blob + imp
    sec = bytearray(0x180)
    struct.pack_into(">I", sec, 4, 0x10000)
    return body + bytes(sec)


class XexHeaderTests(unittest.TestCase):
    def test_identity_fields(self):
        h = iso_extract.parse_xex_header(xex_header(disc=2, discs=3, flags=0x9))
        self.assertEqual(h["execution_info"]["title_id"], "0x41410001")
        self.assertEqual(h["execution_info"]["media_id"], "0x12345678")
        self.assertEqual((h["execution_info"]["disc_number"], h["execution_info"]["disc_count"]), (2, 3))
        self.assertEqual(h["module_kinds"], ["title", "dll"])
        self.assertEqual(h["original_pe_name"], "default.exe")
        self.assertEqual([lib["name"] for lib in h["import_libraries"]], ["xboxkrnl.exe", "xam.xex"])
        self.assertEqual(h["import_libraries"][0]["version"], "2.0.7645.0")
        self.assertEqual(h["image_size"], 0x10000)

    def test_other_magic_and_truncation(self):
        self.assertEqual(iso_extract.parse_xex_header(b"XEX1" + b"\0" * 40)["supported"], False)
        with self.assertRaises(ValueError):
            iso_extract.parse_xex_header(xex_header()[:60])
        with self.assertRaises(ValueError):
            iso_extract.parse_xex_header(b"XEX2" + b"\0" * 10)

    def test_listing_parse_both_separators(self):
        text = ("extract-xiso v2.7.1\n\nlisting x.iso:\n\n\\$SystemUpdate\\ (0 bytes)\n"
                "\\$SystemUpdate\\su.bin (10 bytes)\n/default.xex (20 bytes)\n/a dir (1)/f (x) (5 bytes)\n\n"
                "3 files in x.iso total 35 bytes\n")
        files, dirs, declared = iso_extract.parse_listing(text)
        self.assertEqual(files, {"$SystemUpdate/su.bin": 10, "default.xex": 20, "a dir (1)/f (x)": 5})
        self.assertEqual(dirs, 1)
        self.assertEqual(declared, (3, 35))

    def test_prune_refuses_unmarked_tree(self):
        out = TOOLS.parent / "build" / "prime-aggregate-tests"
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as d:
            disc = Path(d)
            (disc / "default.xex").write_bytes(b"x")
            (disc / "data").mkdir()
            (disc / "data" / "a.bin").write_bytes(b"y")
            with self.assertRaises(ValueError):
                iso_extract.prune(disc, {"default.xex"})
            (disc / ".rcomp-extracted").write_text("h\n")
            iso_extract.prune(disc, {"default.xex"})
            self.assertEqual(sorted(p.name for p in disc.rglob("*")), [".rcomp-extracted", "default.xex"])


class CatalogFixture:
    def __init__(self):
        out = TOOLS.parent / "build" / "prime-aggregate-tests"
        out.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=out)
        self.root = Path(self.tmp.name)

    def add(self, ident, title, media, missing=(), variables=(), unknown=(), disc=1, discs=1, status="PASS",
            xex=True, recompile=None, analyse_exit=0, helpers=None, implemented=()):
        e = self.root / ident
        e.mkdir()
        header = {"execution_info": {"title_id": title, "media_id": media, "disc_number": disc, "disc_count": discs},
                  "import_libraries": [{"name": "xboxkrnl.exe", "version": "2.0.6683.0", "min_version": "2.0.6683.0"}], "supported": True}
        flags = {"multi_disc": discs > 1, "install_disc_suspected": not xex}
        (e / "disc.json").write_text(json.dumps({
            "status": status, "reason": None if status == "PASS" else "insufficient free space", "date": "2026-10-02T00:00:00",
            "iso": {"file_name": f"Secret Game {ident}.iso"}, "disc": {"layout": "XGD2"},
            "default_xex": {"present": xex, "path": "default.xex", "header": header}, "flags": flags}))

        def inventory(recomp):
            mods = {}
            for name in list(missing) + list(variables) + list(unknown) + list(implemented):
                mods.setdefault(name.split(":")[0], {"functions_missing": [], "variables_missing": [],
                                                    "unknown_ordinals": [], "functions_implemented": []})
            for name in missing:
                m, n = name.split(":")
                mods[m]["functions_missing"].append(n)
            for name in variables:
                m, n = name.split(":")
                mods[m]["variables_missing"].append(n)
            for name in unknown:
                m, n = name.split(":")
                mods[m]["unknown_ordinals"].append(n)
            for name in implemented:
                m, n = name.split(":")
                mods[m]["functions_implemented"].append(n)
            return {"decode": {"supported": True}, "imports": mods,
                    "register_helpers": helpers or {"savegprlr_14_address": {"matches": 1}},
                    "jump_tables": {"xenonanalyse_exit": analyse_exit, "tables": 3 if analyse_exit == 0 else None},
                    "xenonrecomp": recomp or {"ran": False}}
        if status == "PASS" and xex:
            (e / "inventory").mkdir()
            (e / "inventory" / "inventory.json").write_text(json.dumps(inventory(None)))
            (e / "inventory.run.json").write_text(json.dumps({"status": "PASS", "date": "2026-10-02T00:00:00"}))
            if recompile is not None:
                (e / "inventory-recompile").mkdir()
                (e / "inventory-recompile" / "inventory.json").write_text(json.dumps(inventory(recompile)))
                (e / "inventory-recompile.run.json").write_text(json.dumps({"status": "PASS", "date": "2026-10-02T00:00:00"}))
        return e


CLEAN = {"ran": True, "exit_code": 0, "functions": 10, "warnings": {}, "unrecognized_instructions": {}}


class AggregateTests(unittest.TestCase):
    def setUp(self):
        self.c = CatalogFixture()

    def tearDown(self):
        self.c.tmp.cleanup()

    def run_agg(self, evidence=None):
        return agg.aggregate(self.c.root, evidence or {})

    def by_key(self, result):
        return {t["key"]: t for t in result["titles"]}

    def test_stages_and_ranking(self):
        self.c.add("a", "0x00000001", "0x0000000A", missing=["xam.xex:XamA"], recompile=CLEAN)
        self.c.add("b", "0x00000002", "0x0000000B", missing=["xam.xex:XamA", "xboxkrnl.exe:KeB"])
        self.c.add("c", "0x00000003", "0x0000000C", missing=["xboxkrnl.exe:KeB"], variables=["xboxkrnl.exe:VarC"])
        self.c.add("d", "0x00000004", "0x0000000D", recompile=CLEAN)
        r = self.run_agg()
        t = self.by_key(r)
        self.assertEqual(t["0x00000001:0x0000000A:1"]["stage"], "recompile")
        self.assertEqual(t["0x00000002:0x0000000B:1"]["stage"], "analyse")
        self.assertEqual(t["0x00000004:0x0000000D:1"]["status"], "PASS")
        self.assertEqual(t["0x00000004:0x0000000D:1"]["label"], "recompiled")
        self.assertEqual(t["0x00000002:0x0000000B:1"]["status"], "FAIL")
        ranked = {row["import"]: row for row in r["imports_ranked"]}
        self.assertEqual((ranked["xam.xex:XamA"]["blocks"], ranked["xam.xex:XamA"]["completes"]), (2, 1))
        self.assertEqual((ranked["xboxkrnl.exe:KeB"]["blocks"], ranked["xboxkrnl.exe:KeB"]["completes"]), (2, 0))
        self.assertEqual(r["imports_ranked"][0]["import"], "xam.xex:XamA")  # tie on blocks broken by completes
        self.assertEqual([n["blockers"] for n in r["near_ready"]], [0, 1, 2, 2])
        self.assertEqual(r["recompile_order"][0], "d")
        first = r["greedy_plan"][0]
        self.assertEqual(first["adds"], ["xam.xex:XamA"])
        self.assertIn("0x00000001:0x0000000A:1", first["titles_import_complete"])

    def test_parking_tags(self):
        self.c.add("k", "0x00000005", "0x0000000E", missing=["xam.xex:XamA"], implemented=["xam.xex:XamNuiCameraAdjustTilt"])
        self.c.add("m", "0x00000006", "0x0000000F", discs=2)
        self.c.add("i", "0x00000007", "0x00000010", xex=False)
        self.c.add("o", "0x00000008", "0x00000011", missing=["xam.xex:XamA"])
        ev = {"titles": {"0x00000008:0x00000011:1": {"tags": ["online-only"]}}}
        r = self.run_agg(ev)
        t = {x["id"]: x for x in r["titles"]}
        self.assertEqual((t["k"]["status"], t["k"]["reason"]), ("BLOCKED", "Kinect title"))
        self.assertEqual(t["m"]["status"], "BLOCKED")
        self.assertIn("multi-disc", t["m"]["reason"])
        self.assertEqual(t["i"]["status"], "BLOCKED")
        self.assertEqual(t["o"]["reason"], "online-only title")
        self.assertEqual(r["imports_ranked"], [])  # parked titles never drive the ranking

    def test_failures_are_recorded_not_skipped(self):
        self.c.add("f", "0x00000009", "0x00000012", status="FAIL")
        self.c.add("g", "0x0000000A", "0x00000013", analyse_exit=124)
        self.c.add("h", "0x0000000B", "0x00000014", recompile={"ran": True, "exit_code": 124, "functions": 0,
                                                              "warnings": {}, "unrecognized_instructions": {}})
        self.c.add("u", "0x0000000C", "0x00000015", recompile={"ran": True, "exit_code": 0, "functions": 5,
                   "warnings": {"switch_without_table": 2, "functions_ending_prematurely": 1},
                   "unrecognized_instructions": {"vrlimi128": 3}},
                   helpers={"savegprlr_14_address": {"matches": 2}})
        r = self.run_agg()
        t = {x["id"]: x for x in r["titles"]}
        self.assertEqual((t["f"]["status"], t["f"]["stage"]), ("FAIL", None))
        self.assertIn("insufficient free space", t["f"]["reason"])
        self.assertEqual(t["g"]["status"], "FAIL")
        self.assertIn("timeout", t["g"]["reason"])
        self.assertIn("generator timeout", t["h"]["signatures"])
        self.assertEqual(t["u"]["signatures"], ["function bounds", "register helpers", "switch tables",
                                                "unimplemented instruction: vrlimi128"])
        groups = {g["signature"]: g for g in r["signatures"]}
        self.assertIn("setjmp/longjmp", groups)
        self.assertTrue(groups["setjmp/longjmp"]["note"].startswith("NOT TESTED"))
        self.assertEqual(groups["switch tables"]["titles"], ["0x0000000C:0x00000015:1"])

    def test_not_run_is_not_tested(self):
        self.c.add("n", "0x0000000D", "0x00000016")
        t = self.run_agg()["titles"][0]
        self.assertEqual((t["stage"], t["status"]), ("analyse", "NOT TESTED"))

    def test_evidence_stage_and_docs_are_sanitized(self):
        self.c.add("p", "0x0000000E", "0x00000017", recompile=CLEAN)
        ev = {"titles": {"0x0000000E:0x00000017:1": {"engine": "Engine X", "stages": {
            "PS5 boot": {"status": "PASS", "reference": "docs/X.md"},
            "rendering": {"status": "NOT TESTED", "reference": "docs/X.md"}}}}}
        r = self.run_agg(ev)
        self.assertEqual(r["titles"][0]["label"], "PS5 boot")
        docs = self.c.root / "COMPATIBILITY.md"
        agg.write_docs(r, docs, "2026-10-02")
        text = docs.read_text(encoding="utf-8")
        self.assertIn("| 0x0000000E | 0x00000017 | 1/1 | Engine X | PS5 boot | PASS |", text)
        self.assertIn("rendering: NOT TESTED", text)
        self.assertNotIn("Secret Game", text)
        agg.write_report(r, self.c.root / "REPORT.md", None)
        self.assertIn("Secret Game", (self.c.root / "REPORT.md").read_text(encoding="utf-8"))
        self.assertEqual(agg.sanitized_reason("no inventory.json: C:\\Users\\x\\y.xex failed"),
                         "no inventory.json: <path> failed")

    def test_kernel_profile_blocker_mirrors_title_create(self):
        lib = lambda name, v, m: {"name": name, "version": v, "min_version": m}
        same = [lib("xboxkrnl.exe", "2.0.6683.0", "2.0.6683.0"), lib("xam.xex", "2.0.6683.0", "2.0.6683.0")]
        self.assertIsNone(agg.kernel_profile_blocker(same, {agg.packed_version("2.0.6683.0")}))
        self.assertIn("not an owner-approved", agg.kernel_profile_blocker(same, {0x20213200}))
        newer = [lib("xboxkrnl.exe", "2.0.7645.0", "2.0.6717.0")]
        self.assertIn("differs from its minimum", agg.kernel_profile_blocker(newer, None))
        mixed = [lib("xboxkrnl.exe", "2.0.6683.0", "2.0.6683.0"), lib("xam.xex", "2.0.8498.0", "2.0.8498.0")]
        self.assertIn("minimums differ", agg.kernel_profile_blocker(mixed, None))
        self.assertEqual(agg.packed_version("2.0.6683.0"), 0x201A1B00)
        # None once the runtime accepts a range of kernel versions (rule-based profile_is_supported) instead of a table
        profiles = agg.supported_kernel_profiles()
        self.assertTrue(profiles is None or 0x201A1B00 in profiles)

    def test_compare_reports_stage_changes(self):
        self.c.add("a", "0x00000001", "0x0000000A", missing=["xam.xex:XamA"])
        before = json.loads(json.dumps(self.run_agg(), default=sorted))
        self.c.tmp.cleanup()
        self.c = CatalogFixture()
        self.c.add("a", "0x00000001", "0x0000000A", recompile=CLEAN)
        after = self.run_agg()
        self.assertEqual(agg.compare(before, after), [("0x00000001:0x0000000A:1", "inventory", "recompiled", 1, 0)])


if __name__ == "__main__":
    unittest.main()
