#!/usr/bin/env python3
"""Symbolize bounded PS5 host crash addresses against that build's LLD map."""
import argparse
import bisect
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map", type=Path)
    parser.add_argument("log", type=Path)
    parser.add_argument("--load-base", type=lambda x: int(x, 0), default=0x400000)
    args = parser.parse_args()
    rows = []
    for line in args.map.read_text(errors="replace").splitlines():
        match = re.match(r"\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+)\s+(.*)$", line)
        if not match:
            continue
        address, size, name = int(match[1], 16), int(match[3], 16), match[5].strip()
        if size and not name.startswith(("/", "<", ".")):
            rows.append((address, size, name))
    rows.sort()
    keys = [row[0] for row in rows]
    for line in args.log.read_text(errors="replace").splitlines():
        match = re.search(r"RCOMP-CRASH host_stack\[(\d+)\] (\w+)=0x([0-9a-fA-F]+)", line)
        if not match:
            continue
        pc = int(match[3], 16)
        # A call's return points just after the call, including at a function end.
        relative = pc - args.load_base - 1
        index = bisect.bisect_right(keys, relative) - 1
        if index >= 0 and relative < rows[index][0] + rows[index][1]:
            address, _, name = rows[index]
            symbol = f"{name}+0x{pc - args.load_base - address:x}"
        else:
            symbol = "NOT TESTED: outside mapped symbol ranges"
        print(f"host_stack[{match[1]}] {match[2]}=0x{pc:x}: {symbol}")


if __name__ == "__main__":
    main()
