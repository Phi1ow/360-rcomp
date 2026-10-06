#!/usr/bin/env python3
"""Reject malformed original module metadata before entering the PPC program."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def u32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def malformed_packages(original):
    count = u32(original, 20)
    slots = {u32(original, 24 + i * 8): 24 + i * 8 for i in range(count)}
    assert {0xCAFE0000, 0xCAFE0202, 0xCAFE03FF} <= slots.keys()
    for name in ("duplicate_key", "fixed_block_overlaps_table", "fixed_block_outside_header",
                 "variable_block_size_overflow"):
        data = bytearray(original)
        if name == "duplicate_key":
            struct.pack_into(">I", data, slots[0xCAFE0202], 0xCAFE0000)
        elif name == "fixed_block_overlaps_table":
            struct.pack_into(">I", data, slots[0xCAFE0202] + 4, 24)
        elif name == "fixed_block_outside_header":
            struct.pack_into(">I", data, slots[0xCAFE0202] + 4, u32(data, 8) - 4)
        else:
            struct.pack_into(">I", data, u32(data, slots[0xCAFE03FF] + 4), 0xFFFFFFFC)
        yield name, data

    # Preserve the type-0 import record but request an unsupported variable.
    # The loader must retain its poison/rejection policy for XboxKrnlVersion.
    data = bytearray(original)
    image_offset = u32(data, 8)
    image_base = u32(data, slots[0x00010201] + 4)
    imports = u32(data, slots[0x000103FF] + 4)
    library = imports + 12 + u32(data, imports + 4)
    count = struct.unpack_from(">H", data, library + 0x26)[0]
    replaced = 0
    for i in range(count):
        address = u32(data, library + 0x28 + i * 4)
        offset = image_offset + address - image_base
        if u32(data, offset) == 0x00000193:
            struct.pack_into(">I", data, offset, 0x00000158)
            replaced += 1
    assert replaced == 1
    yield "unsupported_variable_stays_poisoned", data


def main():
    binary, source, build = map(Path, sys.argv[1:])
    cases = [("missing_xex", None), ("wrong_xex", b"invalid original module fixture")]
    cases += list(malformed_packages(source.read_bytes()))
    for case, data in cases:
        with tempfile.TemporaryDirectory(prefix="modules " + case + " ", dir=build) as tmp:
            xex = Path(tmp) / "rcomp_modules.xex"
            if data is not None:
                xex.write_bytes(data)
            result = subprocess.run([str(binary), str(xex)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 1, (case, result.returncode, result.stdout, result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            expected = "packaged_xex_read" if data is None else "title_create"
            assert any(row["id"] == "xex/modules/" + expected and row["status"] == "FAIL" for row in rows), result.stdout
            assert "stage=entry_begin" not in result.stdout, result.stdout
            if case == "unsupported_variable_stays_poisoned":
                assert "name=XboxKrnlVersion" in result.stdout and "poison=" in result.stdout, result.stdout
                assert "unresolved variable imports" in result.stdout, result.stdout
            assert "RCOMP-MODULES" in result.stdout, result.stdout
            print("modules/" + case + " PASS")


if __name__ == "__main__":
    main()
