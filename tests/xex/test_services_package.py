#!/usr/bin/env python3
"""Production TitleRuntime service runner rejects missing or malformed fixture XEX packages."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main() -> None:
    binary, source_xex, build = map(Path, sys.argv[1:])
    for case in ("missing_xex", "wrong_xex", "missing_data", "wrong_data"):
        with tempfile.TemporaryDirectory(prefix="services " + case + " ", dir=build) as tmp:
            xex = Path(tmp) / "rcomp_services.xex"
            if case == "wrong_xex":
                xex.write_bytes(b"invalid original services fixture")
            elif case in ("missing_data", "wrong_data"):
                shutil.copyfile(source_xex, xex)
                if case == "wrong_data":
                    (Path(tmp) / "boot-data.bin").write_bytes(b"\x01\x02\x03\x04\x05")
            result = subprocess.run([str(binary), str(xex)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 1, (case, result.returncode, result.stdout, result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            if case == "wrong_xex":
                expected = "title_create"
            elif case == "missing_xex":
                expected = "packaged_xex_read"
            else:
                expected = "ppc_service_oracle"
            assert any(row["id"] == "xex/services/" + expected and row["status"] == "FAIL"
                       for row in rows), result.stdout
            if case in ("missing_xex", "wrong_xex"):
                assert "stage=entry_begin" not in result.stdout, result.stdout
            else:
                assert "exit=0x0000E508" in result.stdout, result.stdout
            assert "RCOMP-SERVICES" in result.stdout, result.stdout
            print("services/" + case + " PASS")


if __name__ == "__main__":
    main()
