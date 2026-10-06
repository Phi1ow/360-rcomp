#!/usr/bin/env python3
"""Host checks of tools/scan_register_inputs.py on small pieces of generated code (no game data)."""
import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("scan_register_inputs", ROOT / "tools/scan_register_inputs.py")
scan_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scan_tool)


def flagged(*body):
    return scan_tool.scan_function(list(body))


class RegisterInputScanner(unittest.TestCase):
    def test_a_non_volatile_register_read_before_it_is_written_is_an_input(self):
        found = flagged("\tctx.r16.u64 = PPC_LOAD_U32(ctx.r18.u32 + 4);", "\tctx.r18.u64 = 0;")
        self.assertEqual(list(found), ["r18"])
        self.assertEqual(scan_tool.classify("r18"), "non_volatile")
        self.assertEqual(scan_tool.classify("r12"), "non_argument")
        self.assertEqual(scan_tool.classify("r3"), None)

    def test_writing_first_makes_it_a_local_value(self):
        self.assertEqual(flagged("\tctx.r18.u64 = ctx.r3.u64;", "\tctx.r16.u64 = PPC_LOAD_U32(ctx.r18.u32 + 4);"), {})

    def test_the_spill_of_a_prologue_is_not_an_input(self):
        self.assertEqual(flagged("\tPPC_STORE_U64(ctx.r1.u32 + -152, ctx.r14.u64);",
                                 "\tPPC_STORE_U64(ctx.r1.u32 + -56, ctx.f31.u64);"), {})

    def test_unreachable_jump_table_words_are_skipped_until_the_next_label(self):
        body = ["\tctx.r11.u64 = 1;", "\tswitch (ctx.r11.u32) {", "\tcase 0:", "\t\tgoto loc_1;", "\tdefault:",
                "\t\t__builtin_unreachable();", "\t}",
                "\tctx.r16.u64 = PPC_LOAD_U32(ctx.r18.u32 + 9884);",   # a table word decoded as lwz
                "loc_1:", "\tctx.r3.u64 = ctx.r19.u64;"]
        self.assertEqual(list(flagged(*body)), ["r19"])

    def test_condition_registers_ctr_and_the_reservation(self):
        self.assertEqual(list(flagged("\tif (ctx.cr6.eq) goto loc_1;")), ["cr6"])
        self.assertEqual(flagged("\tctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);", "\tif (ctx.cr6.eq) goto loc_1;"), {})
        self.assertEqual(list(flagged("\t--ctx.ctr.u64;")), ["ctr"])
        self.assertEqual(list(flagged("\tctx.cr0.so = ctx.xer.so;")), ["xer"])   # the record form copies XER[SO]
        self.assertEqual(list(flagged("\tif (ctx.reserved.u32 == 1) goto loc_1;")), ["reserved"])
        self.assertEqual(scan_tool.classify("cr6"), "condition")
        self.assertEqual(scan_tool.classify("ctr"), "condition")

    def test_the_condition_register_save_reads_all_eight_and_is_ignored(self):
        mfcr = "\tctx.r12.u64 = " + " | ".join(f"(ctx.cr{n}.lt << {n})" for n in range(8)) + ";"
        self.assertEqual(flagged(mfcr), {})

    def test_a_vector_store_through_a_cast_pointer_is_a_write(self):
        self.assertEqual(flagged("\tsimde_mm_store_si128((simde__m128i*)ctx.v30.u8, simde_mm_setzero_si128());",
                                 "\tctx.r3.u64 = ctx.v30.u32[0];"), {})

    def test_helpers_are_counted_apart_and_a_whole_directory_is_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            text = "\n".join([
                "PPC_FUNC_IMPL(__imp__sub_82122638) {", "\tctx.r16.u64 = PPC_LOAD_U32(ctx.r18.u32 + 4);", "}",
                "PPC_FUNC_IMPL(__imp____restvmx_100) {", "\tctx.r3.u64 = ctx.r12.u64;", "}",
                "PPC_FUNC_IMPL(__imp__sub_82122700) {", "\tctx.r3.u64 = 1;", "}", ""])
            Path(tmp, "ppc_recomp.0.cpp").write_text(text, encoding="utf-8")
            out = io.StringIO()
            old_argv = sys.argv
            sys.argv = ["scan_register_inputs.py", tmp]
            try:
                with contextlib.redirect_stdout(out):
                    scan_tool.main()
            finally:
                sys.argv = old_argv
        report = out.getvalue()
        self.assertIn("3 functions scanned", report)
        self.assertIn("non_volatile: 1 functions read a register before writing it", report)
        self.assertIn("non_argument: 0 functions read a register before writing it (and 1 __save/__rest helpers", report)


if __name__ == "__main__":
    sys.exit(unittest.main(verbosity=2))
