#!/usr/bin/env python3
"""Black frames of a PS5 run from the RCOMP-LUMA lines of the black-frame probe (RCOMP_LUMA_PROBE=1 in the title's environment; gpu/xenos/rexglue/rcomp/vulkan_display.cpp).

usage: luma_report.py <rcomp_title.err> [<rcomp_title.err> ...] [--bucket SECONDS] [--flash-frames N]

The probe copies sixteen 8x8 tiles of every presented guest image and logs the mean and maximum luma (0..255) of the 1,024 texels when they move by more than 25 % and 6 levels from the last
logged frame, and for every black frame (mean < 3 and maximum < 12). A *black run* is consecutive black frames; a *flash* is a black run of at most --flash-frames frames (default 3, that is
50 ms at 60 Hz) between two frames that are not dark (the last logged mean before it and the first logged mean after it are both above 12). The game draws black frames itself (fades, loading
screens): compare arms by the same scene windows, not by the totals.
"""
import argparse
import re
from pathlib import Path

LINE = re.compile(r"RCOMP-LUMA f=(\d+) t=(\d+) m=([\d.]+) x=(\d+)( BLACK)?")
STATS = re.compile(r"RCOMP-LUMA-STATS presents=(\d+) black=(\d+) dark=(\d+) logged=(\d+)")


def runs_of(events, flash_frames):
    """events: list of (frame, t_ms, mean, max, black) in order. Returns (black runs, flashes, last stats)."""
    runs = []
    current = None
    for i, (frame, t_ms, mean, maximum, black) in enumerate(events):
        if black:
            if current and frame == current["end"] + 1:
                current["end"] = frame
                current["t_end"] = t_ms
            else:
                if current:
                    runs.append(current)
                before = events[i - 1][2] if i else None
                current = {"start": frame, "end": frame, "t_start": t_ms, "t_end": t_ms, "before": before, "after": None}
        elif current:
            current["after"] = mean
            runs.append(current)
            current = None
    if current:
        runs.append(current)
    flashes = [r for r in runs if r["end"] - r["start"] + 1 <= flash_frames and r["before"] is not None and r["after"] is not None and r["before"] > 12 and r["after"] > 12]
    return runs, flashes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--bucket", type=float, default=30.0, help="seconds per bucket of the black frame histogram")
    parser.add_argument("--flash-frames", type=int, default=3)
    args = parser.parse_args()
    for path in args.logs:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        events = [(int(m.group(1)), int(m.group(2)), float(m.group(3)), int(m.group(4)), bool(m.group(5))) for m in LINE.finditer(text)]
        stats = STATS.findall(text)
        print(f"== {path}")
        if not events:
            print("   no RCOMP-LUMA line (probe off, or no frame presented)")
            continue
        if stats:
            presents, black, dark, logged = (int(x) for x in stats[-1])
            print(f"   last stats: presents={presents} black={black} dark={dark} logged={logged}")
        runs, flashes = runs_of(events, args.flash_frames)
        print(f"   black runs: {len(runs)}; flashes (black runs of <= {args.flash_frames} frames between lit frames): {len(flashes)}")
        buckets = {}
        for frame, t_ms, mean, maximum, black in events:
            if black:
                buckets[int(t_ms / 1000.0 // args.bucket)] = buckets.get(int(t_ms / 1000.0 // args.bucket), 0) + 1
        if buckets:
            print("   black frames per %.0f s bucket (seconds since the first present): " % args.bucket + ", ".join(f"{int(k * args.bucket)}-{int((k + 1) * args.bucket)}: {v}" for k, v in sorted(buckets.items())))
        for r in flashes[:60]:
            print(f"   flash at {r['t_start'] / 1000.0:8.2f} s  frames {r['start']}-{r['end']} ({r['end'] - r['start'] + 1})  before mean {r['before']:.1f}  after mean {r['after']:.1f}")
        if len(flashes) > 60:
            print(f"   ... {len(flashes) - 60} more flashes")


if __name__ == "__main__":
    main()
