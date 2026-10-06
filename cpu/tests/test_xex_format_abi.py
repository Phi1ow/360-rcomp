#!/usr/bin/env python3
"""Synthetic ABI evidence tests; contains no title-derived data."""

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "cpu" / "tools"))

from xex_format_abi import CallAbiScanner  # noqa: E402
from xex_static_metadata import FunctionIndex, ImportRecord, Instruction  # noqa: E402


class FormatAbiTests(unittest.TestCase):
    def test_va_list_pointer_through_local_cell(self) -> None:
        functions = FunctionIndex({0x1000: 0x1100})
        target = ImportRecord("xboxkrnl.exe", 1, "function", 0x14D, "_vsnprintf", 0x3000)
        scanner = CallAbiScanner(functions, {target.address: target})
        sequence = [
            Instruction(0x1000, "std", "r4,0x18(r1)"),
            Instruction(0x1004, "std", "r5,0x20(r1)"),
            Instruction(0x1008, "std", "r6,0x28(r1)"),
            Instruction(0x100C, "std", "r7,0x30(r1)"),
            Instruction(0x1010, "std", "r8,0x38(r1)"),
            Instruction(0x1014, "std", "r9,0x40(r1)"),
            Instruction(0x1018, "std", "r10,0x48(r1)"),
            Instruction(0x101C, "stwu", "r1,-0x170(r1)"),
            Instruction(0x1020, "addi", "r10,r1,0x188"),
            Instruction(0x1024, "stw", "r10,0x50(r1)"),
            Instruction(0x1028, "lwz", "r6,0x50(r1)"),
            Instruction(0x102C, "bl", "3000"),
        ]
        for instruction in sequence:
            scanner.feed(instruction)

        self.assertEqual(len(scanner.calls), 1)
        call = scanner.calls[0]
        self.assertEqual(call["frame_size_at_call"], 0x170)
        self.assertIn({"register": 6, "stack_offset": 0x188}, call["argument_stack_pointers"])
        writes = call["recent_stack_writes"]
        self.assertTrue(
            any(
                write["stack_offset"] == 0x188
                and write["width"] == 8
                and write["source_register"] == 4
                for write in writes
            )
        )
        self.assertTrue(
            any(
                write["stack_offset"] == 0x50
                and write["width"] == 4
                and write["stored_stack_pointer_offset"] == 0x188
                for write in writes
            )
        )

    def test_first_overflow_slot_can_be_eight_bytes_at_0x50(self) -> None:
        functions = FunctionIndex({0x2000: 0x2100})
        target = ImportRecord("xboxkrnl.exe", 1, "function", 0x13B, "sprintf", 0x3010)
        scanner = CallAbiScanner(functions, {target.address: target})
        for instruction in (
            Instruction(0x2000, "stwu", "r1,-0x100(r1)"),
            Instruction(0x2004, "std", "r11,0x50(r1)"),
            Instruction(0x2008, "bl", "3010"),
        ):
            scanner.feed(instruction)
        write = scanner.calls[0]["recent_stack_writes"][0]
        self.assertEqual((write["stack_offset"], write["width"]), (0x50, 8))


if __name__ == "__main__":
    unittest.main()
