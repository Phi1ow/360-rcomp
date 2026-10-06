#!/usr/bin/env python3
"""Exercise the actual packaged runner's refusal of incomplete original data."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary, fixture, build = map(Path, sys.argv[1:])
    for case in ("missing_data", "wrong_data", "wrong_xex"):
        with tempfile.TemporaryDirectory(prefix="packaged boot " + case + " ", dir=build) as tmp:
            directory = Path(tmp)
            xex = directory / "rcomp_title.xex"
            xex.write_bytes(b"invalid original test XEX" if case == "wrong_xex" else fixture.read_bytes())
            if case != "missing_data":
                (directory / "boot-data.bin").write_bytes(b"\x01\x02\x03\x05" if case == "wrong_data" else b"\x01\x02\x03\x04")
            result = subprocess.run([str(binary), str(xex)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 1, (case, result.returncode, result.stdout, result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            expected = "title_create" if case == "wrong_xex" else "packaged_data_exact"
            assert any(row["id"] == "xex/packaged_boot/" + expected and row["status"] == "FAIL" for row in rows), result.stdout
            assert "stage=entry_begin" not in result.stdout, result.stdout
            assert "RCOMP-BOOT-PACKAGED" in result.stdout, result.stdout
            print("packaged_boot/" + case + " PASS")


if __name__ == "__main__":
    main()
