#!/usr/bin/env python3
"""Create the original four-byte sidecar used by the services PPC fixture."""

import argparse
from pathlib import Path


PAYLOAD = bytes((0x01, 0x02, 0x03, 0x04))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    out = args.out
    if out.exists():
        parser.error(f"refusing to overwrite existing output: {out}")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(PAYLOAD)
    if out.read_bytes() != PAYLOAD:
        raise SystemExit("written services sidecar did not round-trip")
    print(f"SERVICES-DATA bytes={len(PAYLOAD)} hex={PAYLOAD.hex()} path={out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
