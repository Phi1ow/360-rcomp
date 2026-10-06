#!/usr/bin/env python3
"""Summarize measured MFTB/native-clock and Xenos vblank observer windows."""
import argparse
import datetime
import json
import re
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("log", type=Path)
    p.add_argument("--out", type=Path)
    p.add_argument("--last-seconds", type=float)
    a = p.parse_args()
    observations = []
    failures = []
    incomplete = []
    required = {"mono_now_ns", "real_now_ns", "mono_ns", "guest_tb_delta",
                "guest_seconds_per_mono", "tsc_delta", "vblanks_delta",
                "vblank_hz", "swaps_delta"}
    for line in a.log.read_text(errors="replace").replace("\x00", "").splitlines():
        if "RCOMP-TIMEBASE FAIL" in line:
            failures.append(line)
        if "RCOMP-TIMEBASE " not in line or "mono_now_ns=" not in line:
            continue
        row = {k: float(v) if "." in v else int(v) for k, v in
               re.findall(r"(\w+)=(-?[0-9]+(?:\.[0-9]+)?)", line)}
        missing = required - row.keys()
        if missing or row.get("mono_ns", 0) <= 0:
            incomplete.append({"mono_now_ns": row.get("mono_now_ns"),
                               "missing_fields": sorted(missing),
                               "line": line})
            continue
        observations.append(row)
    if not observations:
        p.error("NOT TESTED: no observer windows")
    if a.last_seconds is not None:
        if a.last_seconds <= 0:
            p.error("last-seconds must be positive")
        earliest = observations[-1]["mono_now_ns"] - a.last_seconds * 1e9
        observations = [r for r in observations if r["mono_now_ns"] >= earliest]
        incomplete = [r for r in incomplete
                      if r["mono_now_ns"] is None or r["mono_now_ns"] >= earliest]
    total_ns = sum(r["mono_ns"] for r in observations)
    seconds = total_ns / 1e9
    result = {
        "status": "FAIL" if failures or incomplete else "PASS",
        "scope": "observed clock slope and bridge ticks; simulation speed requires independent evidence",
        "windows": len(observations), "duration_s": seconds,
        "guest_seconds_per_mono": sum(r["guest_tb_delta"] for r in observations) / 50e6 / seconds,
        "guest_ratio_min": min(r["guest_seconds_per_mono"] for r in observations),
        "guest_ratio_max": max(r["guest_seconds_per_mono"] for r in observations),
        "tsc_hz": sum(r["tsc_delta"] for r in observations) / seconds,
        "vblank_hz": sum(r["vblanks_delta"] for r in observations) / seconds,
        "vblank_window_min": min(r["vblank_hz"] for r in observations),
        "vblank_window_max": max(r["vblank_hz"] for r in observations),
        "swap_hz": sum(r["swaps_delta"] for r in observations) / seconds,
        "native_first_utc": datetime.datetime.fromtimestamp(observations[0]["real_now_ns"] / 1e9, datetime.timezone.utc).isoformat(),
        "native_last_utc": datetime.datetime.fromtimestamp(observations[-1]["real_now_ns"] / 1e9, datetime.timezone.utc).isoformat(),
        "failures": failures,
        "incomplete_windows": incomplete,
    }
    if a.out:
        a.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
