#!/usr/bin/env python3
"""Real calendar/thread runner refuses missing or malformed original XEX packages."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary, build = map(Path, sys.argv[1:])
    for case in ("missing_xex", "wrong_xex"):
        with tempfile.TemporaryDirectory(prefix="calendar threads " + case + " ", dir=build) as tmp:
            xex = Path(tmp) / "rcomp_calendar_threads.xex"
            if case == "wrong_xex":
                xex.write_bytes(b"invalid original calendar/thread fixture")
            result = subprocess.run([str(binary), str(xex)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 1, (case, result.returncode, result.stdout, result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            expected = "title_create" if case == "wrong_xex" else "packaged_xex_read"
            assert any(row["id"] == "xex/calendar_threads/" + expected and row["status"] == "FAIL" for row in rows), result.stdout
            assert "stage=entry_begin" not in result.stdout, result.stdout
            assert "RCOMP-CALENDAR-THREADS" in result.stdout, result.stdout
            print("calendar_threads/" + case + " PASS")


if __name__ == "__main__":
    main()
