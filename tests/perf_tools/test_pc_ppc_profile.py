#!/usr/bin/env python3
"""Host checks of tools/pc_ppc_profile.py (no PS5, no llvm-symbolizer): the sample log parser, the mapping from a C++ line of
XenonRecomp output to the PowerPC instruction it implements, and the instruction categories."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("pc_ppc_profile", ROOT / "tools/pc_ppc_profile.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

GENERATED = """#include "ppc_recomp_shared.h"

PPC_FUNC_IMPL(__imp__sub_825C6160) {
	PPC_FUNC_PROLOGUE();
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// lhz r3,3898(r31)
	ctx.r3.u64 = PPC_LOAD_U16(ctx.r31.u32 + 3898);
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(ctx.r3.u32, 0, xer);
	// beq cr6,0x825c6184
	if (cr6.eq) goto loc_825C6184;
loc_825C6184:
	ctx.r11.u64 = 1;
}
"""


class PcLogTests(unittest.TestCase):
    def test_parse_log_sums_windows_and_skips_stack_candidates(self):
        text = ("RCOMP-PC-FORMAT version=2 raw=interrupted-rip rx_begin=400000 rx_end=4c97ce0 rx_valid=1\n"
                "RCOMP-PC main total=10000 recorded=3 dropped=1 48bb626:365 2ae70c7:329\n"
                "RCOMP-PC others total=10000 recorded=1 dropped=0 2ae70c7:9\n"
                "RCOMP-PC main total=10000 recorded=2 dropped=0 48bb626:35 8000000000000123:77\n")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "pc-window.log"
            p.write_text(text)
            counts, rx = tool.parse_log(str(p), "main")
            self.assertEqual(counts[0x48bb626], 400)
            self.assertEqual(counts[0x2ae70c7], 329)
            self.assertNotIn(0x8000000000000123, counts)
            self.assertEqual(rx, (0x400000, 0x4c97ce0))
            others, _ = tool.parse_log(str(p), "others")
            self.assertEqual(dict(others), {0x2ae70c7: 9})

    def test_load_bias_needs_equal_sizes(self):
        with tempfile.TemporaryDirectory() as d:
            m = Path(d) / "title.map"
            m.write_text("     VMA      LMA     Size Align Out     In      Symbol\n"
                         "   400000   400000  4c97ce0    16 .text\n")
            self.assertEqual(tool.load_bias_from_map(str(m), (0x500000, 0x500000 + 0x4c97ce0)), 0x100000)
            self.assertIsNone(tool.load_bias_from_map(str(m), (0x500000, 0x500010)))
            self.assertIsNone(tool.load_bias_from_map(str(m), None))


class InstructionMappingTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "ppc_recomp.100.cpp"
        self.path.write_text(GENERATED)
        self.src = tool.SourceLines()

    def tearDown(self):
        self.dir.cleanup()

    def line_of(self, needle):
        for i, line in enumerate(GENERATED.split("\n"), 1):
            if needle in line:
                return i
        raise AssertionError(needle)

    def test_statement_maps_to_the_comment_above_it(self):
        mn, comment = self.src.instruction_at(str(self.path), self.line_of("PPC_LOAD_U16"))
        self.assertEqual(mn, "lhz")
        self.assertEqual(comment, "lhz r3,3898(r31)")
        mn, _ = self.src.instruction_at(str(self.path), self.line_of("cr6.compare"))
        self.assertEqual(mn, "cmplwi")
        mn, _ = self.src.instruction_at(str(self.path), self.line_of("goto loc_825C6184"))
        self.assertEqual(mn, "beq")

    def test_label_stops_the_search(self):
        self.assertEqual(self.src.instruction_at(str(self.path), self.line_of("ctx.r11.u64 = 1")), (None, None))

    def test_out_of_range_and_missing_file(self):
        self.assertEqual(self.src.instruction_at(str(self.path), 0), (None, None))
        self.assertEqual(self.src.instruction_at(str(self.path), 10000), (None, None))
        self.assertEqual(self.src.instruction_at(str(Path(self.dir.name) / "none.cpp"), 3), (None, None))

    def test_cygwin_paths(self):
        self.assertEqual(tool.host_path("/cygdrive/c/Users/x/ppc_recomp.1.cpp"), "C:/Users/x/ppc_recomp.1.cpp")
        self.assertEqual(tool.host_path("D:/a/b.cpp"), "D:/a/b.cpp")


class CategoryTests(unittest.TestCase):
    def test_categories(self):
        c = tool.category
        self.assertEqual(c("lwz"), "load")
        self.assertEqual(c("stw"), "store")
        self.assertEqual(c("lfs"), "fp load/store")
        self.assertEqual(c("stvx128"), "vector load/store")
        self.assertEqual(c("bl"), "call (bl/bctrl)")
        self.assertEqual(c("bctrl"), "call (bl/bctrl)")
        self.assertEqual(c("blr"), "return/indirect branch")
        self.assertEqual(c("beq"), "branch")
        self.assertEqual(c("cmplwi"), "compare")
        self.assertEqual(c("fmuls"), "fp alu")
        self.assertEqual(c("vmaddfp"), "vector alu")
        self.assertEqual(c("addi"), "integer alu/move")
        self.assertEqual(c(None), "unattributed")


if __name__ == "__main__":
    unittest.main()
