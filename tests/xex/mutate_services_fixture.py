#!/usr/bin/env python3
"""Create the one-line negative mutation of the services PPC oracle."""

import argparse
from pathlib import Path


ORIGINAL = """    ld r0, 0xC8(r1)       # EndOfFile (+40)
    li r11, 4
    cmpd cr0, r0, r11
"""
MUTATED = """    ld r0, 0xC8(r1)       # EndOfFile (+40)
    li r11, 5
    cmpd cr0, r0, r11
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    if args.out.exists():
        parser.error(f"refusing to overwrite existing output: {args.out}")
    text = args.source.read_text()
    if text.count(ORIGINAL) != 1:
        raise SystemExit("expected exactly one full-attributes EOF oracle in source")
    if MUTATED in text:
        raise SystemExit("source already contains the negative mutation")
    changed = text.replace(ORIGINAL, MUTATED, 1)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(changed)
    if args.out.read_text() != changed:
        raise SystemExit("written mutation did not round-trip")
    print("SERVICES-MUTATION check=8 expected-eof=4 mutated-oracle=5 changes=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
