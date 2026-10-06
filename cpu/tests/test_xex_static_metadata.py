#!/usr/bin/env python3
"""Synthetic tests for the metadata-only XEX static-use analyser."""

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "cpu" / "tools"))

from xex_static_metadata import (  # noqa: E402
    CallEdge,
    FunctionIndex,
    ImportRecord,
    Instruction,
    StaticUseScanner,
    compute_reachability,
    direct_target,
    parse_objdump_line,
)


class StaticMetadataTests(unittest.TestCase):
    def test_objdump_line_and_direct_target(self) -> None:
        instruction = parse_objdump_line("  1000: bl 2000")
        self.assertEqual(instruction, Instruction(0x1000, "bl", "2000"))
        self.assertEqual(direct_target(instruction), 0x2000)
        self.assertIsNone(parse_objdump_line("file format binary"))

    def test_variable_slot_field_and_import_argument_tracking(self) -> None:
        functions = FunctionIndex({0x1000: 0x1100})
        variable = ImportRecord("xboxkrnl.exe", 0, "variable", 0x1B, "ThreadType", 0x820006BC)
        callee = ImportRecord("xboxkrnl.exe", 1, "function", 0x110, "ReferenceByHandle", 0x2000)
        scanner = StaticUseScanner(functions, {variable.address: variable}, {callee.address: callee})

        sequence = [
            Instruction(0x1000, "lis", "r11,0x8200"),
            Instruction(0x1004, "lwz", "r4,0x6bc(r11)"),
            Instruction(0x1008, "lhz", "r5,4(r4)"),
            Instruction(0x100C, "lis", "r11,0x8200"),
            Instruction(0x1010, "lwz", "r4,0x6bc(r11)"),
            Instruction(0x1014, "bl", "2000"),
        ]
        for instruction in sequence:
            scanner.feed(instruction)

        self.assertEqual(
            [(r["kind"], r["offset"], r["width"]) for r in scanner.reads],
            [("slot", 0, 4), ("field", 4, 2), ("slot", 0, 4)],
        )
        self.assertEqual(len(scanner.call_uses), 1)
        self.assertEqual(scanner.call_uses[0]["argument_register"], 4)
        self.assertEqual(scanner.call_uses[0]["callee_name"], "ReferenceByHandle")

    def test_reachability_is_direct_call_graph_only(self) -> None:
        calls = [
            CallEdge(0x1000, 0x1004, 0x1100),
            CallEdge(0x1000, 0x1008, 0x1200),
            CallEdge(0x1100, 0x1104, 0x1300),
            CallEdge(0x1400, 0x1404, 0x1500),
        ]
        depth, parent, adjacency = compute_reachability(
            0x1000, calls, {0x1000, 0x1100, 0x1200, 0x1300, 0x1400, 0x1500}
        )
        self.assertEqual(depth, {0x1000: 0, 0x1100: 1, 0x1200: 1, 0x1300: 2})
        self.assertEqual(parent[0x1300], 0x1100)
        self.assertEqual([edge.callsite for edge in adjacency[0x1000]], [0x1004, 0x1008])


if __name__ == "__main__":
    unittest.main()
