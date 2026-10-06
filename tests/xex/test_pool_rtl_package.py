#!/usr/bin/env python3
"""Real pool/Rtl runner refuses missing or malformed original XEX packages."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary, build = map(Path, sys.argv[1:])
    for case in ("missing_xex", "wrong_xex"):
        with tempfile.TemporaryDirectory(prefix="pool rtl " + case + " ", dir=build) as tmp:
            xex = Path(tmp) / "rcomp_pool_rtl.xex"
            if case == "wrong_xex":
                xex.write_bytes(b"invalid original pool/Rtl fixture")
            result = subprocess.run([str(binary), str(xex)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 1, (case, result.returncode, result.stdout, result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            expected = "title_create" if case == "wrong_xex" else "packaged_xex_read"
            assert any(row["id"] == "xex/pool_rtl/" + expected and row["status"] == "FAIL" for row in rows), result.stdout
            assert "stage=entry_begin" not in result.stdout, result.stdout
            assert "RCOMP-POOL-RTL" in result.stdout, result.stdout
            print("pool_rtl/" + case + " PASS")


if __name__ == "__main__":
    main()
