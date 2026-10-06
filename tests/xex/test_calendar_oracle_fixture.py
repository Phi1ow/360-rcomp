#!/usr/bin/env python3
"""Cross-check the committed Windows oracle against the original PPC fixture."""
import hashlib
import json
import re
import sys
from pathlib import Path


def fail(message):
    raise AssertionError(message)


def section(text, start, end=None):
    begin = text.index(start + ":")
    finish = text.index(end + ":", begin) if end else len(text)
    return text[begin:finish]


def short_rows(text):
    rows = []
    for match in re.finditer(r"(?m)^\s*\.short\s+([^#\r\n]+)", text):
        rows.append([int(value.strip(), 0) for value in match.group(1).split(",")])
    return rows


def quad_values(text):
    return [int(value, 0) for value in re.findall(r"(?m)^\s*\.quad\s+([^\s#]+)", text)]


def canonical_sha256(path):
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_calendar_oracle_fixture.py ORACLE ASM GENERATOR")
    oracle_path, asm_path, generator_path = map(Path, sys.argv[1:])
    oracle = json.loads(oracle_path.read_text(encoding="utf-8"))
    asm = asm_path.read_text(encoding="utf-8")

    if oracle.get("scope") != "Windows ntdll calendar oracle, not Xbox/PS5 proof":
        fail("oracle scope changed")
    if oracle.get("fields_size") != 16:
        fail("TIME_FIELDS size must be 16 bytes")
    if oracle.get("source_sha256") != canonical_sha256(generator_path):
        fail("oracle source_sha256 does not match ntdll_calendar_oracle.py")

    success = oracle["fields_to_time"]
    invalid = oracle["invalid_fields"]
    inverse = oracle["time_to_fields"]
    if (len(success), len(invalid), len(inverse)) != (7, 16, 9):
        fail(f"unexpected oracle selection sizes: {len(success)}, {len(invalid)}, {len(inverse)}")

    success_text = section(asm, "calendar_success", "calendar_invalid")
    success_shorts = short_rows(success_text)
    success_quads = quad_values(success_text)
    expected_success_shorts = []
    for row in success:
        if row["return"] != 1 or row["unchanged"] or row["roundtrip"] is None:
            fail(f"invalid success observation: {row}")
        expected_success_shorts.extend([row["fields"], row["roundtrip"]])
    if success_shorts != expected_success_shorts:
        fail("calendar_success .short rows differ from Windows oracle")
    if success_quads != [row["time_signed"] & ((1 << 64) - 1) for row in success]:
        fail("calendar_success .quad values differ from Windows oracle")

    invalid_text = section(asm, "calendar_invalid", "calendar_inverse")
    if short_rows(invalid_text) != [row["fields"] for row in invalid]:
        fail("calendar_invalid rows differ from Windows oracle")
    if any(row["return"] != 0 or not row["unchanged"] or row["roundtrip"] is not None for row in invalid):
        fail("invalid observations must fail and preserve the output sentinel")

    inverse_text = section(asm, "calendar_inverse")
    if short_rows(inverse_text) != [row["fields"] for row in inverse]:
        fail("calendar_inverse fields differ from Windows oracle")
    if quad_values(inverse_text) != [row["time_signed"] & ((1 << 64) - 1) for row in inverse]:
        fail("calendar_inverse ticks differ from Windows oracle")

    imports = re.findall(r"(?m)^#_ XEX_IMPORT\s+\S+\s+\S+\s+\S+", asm)
    if len(imports) != 6:
        fail(f"expected six imported calendar/thread functions, found {len(imports)}")
    thread_checks = sorted({int(value) for value in re.findall(r"# Check(4[0-5]):", asm)})
    if thread_checks != list(range(40, 46)):
        fail(f"thread checks 40..45 are incomplete: {thread_checks}")
    original_checks = 2 * len(success) + len(invalid) + len(inverse) + len(thread_checks)
    if original_checks != 45:
        fail(f"expected 45 original PPC checks, derived {original_checks}")
    if "li r3, 0x70" not in asm:
        fail("fixture success return 0x70 is missing")

    print(
        "CALENDAR-ORACLE checks=45 success=7 invalid=16 inverse=9 thread=6 "
        "imports=6 source_hash=PASS"
    )


if __name__ == "__main__":
    main()
