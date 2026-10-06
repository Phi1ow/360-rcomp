#!/usr/bin/env python3
"""Extracts the dump shader SPIR-V that the title logged (rcomp_diag_log_dump_spirv) into dump_<variant>.spv files of the host test.
usage: extract_dump_spirv.py <rcomp_title.err> <out dir>"""
import re
import struct
import sys
from pathlib import Path

err = Path(sys.argv[1])
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
headers = {}
words = {}
for line in err.read_text(encoding="utf-8", errors="replace").splitlines():
    m = re.match(r"RCOMP-DUMP-SPIRV depth=(\d+) format=(\d+) msaa=(\d+) words=(\d+) (.*)", line)
    if m:
        key = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        headers[key] = (int(m.group(4)), m.group(5))
        continue
    m = re.match(r"RCOMP-DUMP-SPIRV-DATA depth=(\d+) format=(\d+) msaa=(\d+) at=(\d+) (.*)", line)
    if m:
        key = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        at = int(m.group(4))
        chunk = [int(w, 16) for w in m.group(5).split()]
        words.setdefault(key, {})[at] = chunk

MSAA = {0: "1x", 1: "2x", 2: "4x"}
COLOR = {0: "c8888", 1: "c8888gamma", 2: "c2101010", 3: "c2101010f", 6: "c1616f", 14: "c32f"}
DEPTH = {0: "d24s8", 1: "d24fs8"}
for key, (count, facts) in sorted(headers.items()):
    depth, fmt, msaa = key
    name = (DEPTH if depth else COLOR).get(fmt, f"{'d' if depth else 'c'}fmt{fmt}") + "_" + MSAA[msaa]
    chunks = words[key]
    flat = []
    for at in sorted(chunks):
        assert at == len(flat), (key, at, len(flat))
        flat.extend(chunks[at])
    assert len(flat) == count, (key, len(flat), count)
    assert flat[0] == 0x07230203, hex(flat[0])
    (out / f"dump_{name}.spv").write_bytes(struct.pack(f"<{len(flat)}I", *flat))
    print(f"{name}: {count} words  [{facts}]")
