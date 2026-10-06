#!/usr/bin/env python3
"""A decompressed XEX contains memory bytes, not a raw PE file layout.

All inputs are modifications of the original rcomp_modules fixture. No AOT
instruction, import, oracle, XEX loaded extent, or runtime implementation is
modified. The normal runner must still complete every PPC and lifetime check.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def u32(data, offset, endian=">"):
    return struct.unpack_from(endian + "I", data, offset)[0]


def packages(original):
    image = u32(original, 8)
    security = u32(original, 16)
    loaded_size = u32(original, security + 4)
    nt = image + u32(original, image + 0x3C, "<")
    assert original[nt:nt + 4] == b"PE\0\0"
    count = struct.unpack_from("<H", original, nt + 6)[0]
    optional_size = struct.unpack_from("<H", original, nt + 20)[0]
    optional = nt + 24
    sections = optional + optional_size
    assert u32(original, optional + 56, "<") == loaded_size

    data = bytearray(original)
    struct.pack_into("<I", data, optional + 56, loaded_size + 0x2000)
    yield "pe_size_exceeds_loaded_xex", data

    data = bytearray(data)
    extra = sections + count * 40
    assert extra + 40 <= image + 0x1000
    assert data[extra:extra + 40] == bytes(40)
    struct.pack_into("<H", data, nt + 6, count + 1)
    # A logical non-executable section without bytes in the loaded XEX.
    data[extra:extra + 8] = b".extra\0\0"
    struct.pack_into("<IIII", data, extra + 8, 0x1000, loaded_size + 0x1000, 0, 0)
    struct.pack_into("<I", data, extra + 36, 0x40000040)
    yield "nonexecuting_section_beyond_loaded_xex", data

    data = bytearray(original)
    assert data[sections:sections + 8].rstrip(b"\0") == b".text"
    virtual_address = u32(data, sections + 12, "<")
    raw_size = u32(data, sections + 16, "<")
    assert u32(data, sections + 20, "<") == virtual_address
    next_raw = u32(data, sections + 40 + 20, "<")
    new_raw = virtual_address + 0x200
    assert new_raw + raw_size <= next_raw
    struct.pack_into("<I", data, sections + 20, new_raw)
    yield "raw_offset_differs_from_memory_address", data


def main():
    binary, source, build = map(Path, sys.argv[1:])
    for case, data in packages(source.read_bytes()):
        with tempfile.TemporaryDirectory(prefix="module memory " + case + " ", dir=build) as tmp:
            xex = Path(tmp) / "rcomp_modules.xex"
            xex.write_bytes(data)
            result = subprocess.run([str(binary), str(xex)], capture_output=True, text=True, timeout=55)
            (build / ("modules_layout_" + case + ".log")).write_text(result.stdout + result.stderr)
            assert result.returncode == 0, (case, result.returncode, result.stdout, result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            assert len(rows) == 29 and all(row["status"] == "PASS" for row in rows), result.stdout
            assert result.stdout.count("stage=entry_returned code=0x0000006F") == 6, result.stdout
            assert "RCOMP-MODULES checks=29 pass=29 fail=0 scope=host" in result.stdout, result.stdout
            print("modules/" + case + " PASS")


if __name__ == "__main__":
    main()
