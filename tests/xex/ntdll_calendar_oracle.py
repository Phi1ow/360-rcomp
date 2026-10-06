"""Generate independent Windows ntdll calendar observations for the XEX tests.

The committed fixture intentionally contains only documented calendar inputs
used by rcomp_calendar_threads.s.  A deterministic 4,005-vector differential
corpus can also be generated for broad host comparison against the runtime.
No console access and no game input are used.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
import platform
import random
import sys
from pathlib import Path


NAMES = "year month day hour minute second milliseconds weekday".split()
SENTINEL = 0x0123456789ABCDEF
DIFFERENTIAL_SEED = 20260928


class Fields(c.Structure):
    _fields_ = [(name, c.c_int16) for name in NAMES]


FIXTURE_FORWARD = [
    [1601, 1, 1, 0, 0, 0, 0, 0],
    [1970, 1, 1, 0, 0, 0, 0, 0],
    [2000, 2, 29, 12, 34, 56, 789, -123],
    [2400, 2, 29, 0, 0, 0, 0, 0],
    [9999, 12, 31, 23, 59, 59, 999, 0],
    [10000, 1, 1, 0, 0, 0, 0, 0],
    [30827, 12, 31, 23, 59, 59, 999, 0],
    [1900, 2, 29, 0, 0, 0, 0, 0],
    [2100, 2, 29, 0, 0, 0, 0, 0],
    [1600, 12, 31, 23, 59, 59, 999, 0],
]
for field, bad in [
    (0, -1), (1, 0), (1, 13), (2, 0), (2, 32), (3, -1), (3, 24),
    (4, -1), (4, 60), (5, -1), (5, 60), (6, -1), (6, 1000),
]:
    row = [2026, 9, 28, 12, 34, 56, 789, 0]
    row[field] = bad
    FIXTURE_FORWARD.append(row)

FIXTURE_INVERSE = [
    0,
    1,
    9999,
    10000,
    9_999_999,
    10_000_000,
    864_000_000_000 - 1,
    864_000_000_000,
    (1 << 63) - 1,
]


def canonical_source_sha256() -> str:
    text = Path(__file__).read_text(encoding="utf-8").replace("\r\n", "\n")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def observe_forward(to_time, to_fields, row):
    fields = Fields(*row)
    stamp = c.c_int64(SENTINEL)
    ok = int(to_time(c.byref(fields), c.byref(stamp)))
    back = Fields()
    if ok:
        to_fields(c.byref(stamp), c.byref(back))
    return {
        "fields": row,
        "return": ok,
        "time_signed": stamp.value,
        "time_hex": f"{stamp.value & ((1 << 64) - 1):016x}",
        "unchanged": stamp.value == SENTINEL,
        "roundtrip": [getattr(back, name) for name in NAMES] if ok else None,
    }


def observe_inverse(to_fields, value):
    stamp = c.c_int64(value)
    fields = Fields()
    to_fields(c.byref(stamp), c.byref(fields))
    return {
        "time_signed": value,
        "fields": [getattr(fields, name) for name in NAMES],
    }


def fixture_report(to_time, to_fields, ntdll_path: Path):
    forward = [observe_forward(to_time, to_fields, row) for row in FIXTURE_FORWARD]
    success = [row for row in forward if row["return"] == 1]
    invalid = [row for row in forward if row["return"] == 0]
    if len(success) != 7 or len(invalid) != 16:
        raise RuntimeError(
            f"unexpected ntdll fixture split: success={len(success)} invalid={len(invalid)}"
        )
    return {
        "scope": "Windows ntdll calendar oracle, not Xbox/PS5 proof",
        "platform": platform.platform(),
        "ntdll_path": str(ntdll_path),
        "ntdll_sha256": file_sha256(ntdll_path),
        "fields_size": c.sizeof(Fields),
        "source_sha256": canonical_source_sha256(),
        "regenerate": "python tests/xex/ntdll_calendar_oracle.py --out <new-json> (native Windows Python)",
        "fields_to_time": success,
        "invalid_fields": invalid,
        "time_to_fields": [observe_inverse(to_fields, value) for value in FIXTURE_INVERSE],
        "excluded": (
            "Negative inverse inputs and forward year>=30828 are outside this fixture; "
            "runtime diagnoses them as UNIMPLEMENTED rather than copying undocumented ntdll behavior."
        ),
    }


def differential_text(to_time, to_fields):
    randomizer = random.Random(DIFFERENTIAL_SEED)
    dates = []
    for year in range(1601, 2001):
        dates.extend([
            [year, 2, 29, 23, 59, 59, 999, -99],
            [year, 3, 1, 0, 0, 0, 0, 99],
        ])
    for _ in range(1200):
        dates.append([
            randomizer.randint(1601, 30827),
            randomizer.randint(1, 12),
            randomizer.randint(1, 31),
            randomizer.randrange(24),
            randomizer.randrange(60),
            randomizer.randrange(60),
            randomizer.randrange(1000),
            randomizer.randint(-32768, 32767),
        ])

    lines = []
    for row in dates:
        observation = observe_forward(to_time, to_fields, row)
        lines.append(
            "F " + " ".join(map(str, row))
            + f" {observation['return']} {observation['time_signed']}\n"
        )

    inverse_values = [0, 1, 9999, 10000, (1 << 63) - 1]
    inverse_values.extend(randomizer.randrange(1 << 63) for _ in range(2000))
    for value in inverse_values:
        observation = observe_inverse(to_fields, value)
        lines.append(
            f"I {value} " + " ".join(map(str, observation["fields"])) + "\n"
        )

    if len(lines) != 4005:
        raise RuntimeError(f"unexpected differential vector count: {len(lines)}")
    return "".join(lines)


def write_new(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as target:
        target.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, help="write the selected fixture JSON; existing files are refused")
    parser.add_argument(
        "--differential-out",
        type=Path,
        help="write the deterministic 4,005-vector corpus; existing files are refused",
    )
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("this independent oracle requires native Windows Python and ntdll.dll")

    ntdll_path = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "ntdll.dll"
    nt = c.WinDLL(str(ntdll_path))
    to_time = nt.RtlTimeFieldsToTime
    to_time.argtypes = [c.POINTER(Fields), c.POINTER(c.c_int64)]
    to_time.restype = c.c_ubyte
    to_fields = nt.RtlTimeToTimeFields
    to_fields.argtypes = [c.POINTER(c.c_int64), c.POINTER(Fields)]
    to_fields.restype = None

    report = fixture_report(to_time, to_fields, ntdll_path)
    encoded = json.dumps(report, indent=2) + "\n"
    if args.out:
        write_new(args.out, encoded)
    else:
        print(encoded, end="")

    if args.differential_out:
        differential = differential_text(to_time, to_fields)
        write_new(args.differential_out, differential)
        digest = hashlib.sha256(differential.encode("utf-8")).hexdigest()
        print(
            f"NTDLL-DIFFERENTIAL-VECTORS checks=4005 seed={DIFFERENTIAL_SEED} sha256={digest}",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
