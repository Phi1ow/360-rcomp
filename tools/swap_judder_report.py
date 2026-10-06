#!/usr/bin/env python3
"""Frame pacing of console runs from the per-second RCOMP-FPS lines of rcomp_title.err: the share of the guest's swaps that came 1, 2 or 3+ vertical blanks after the previous one, per slice of the run
(a swap that comes two vblanks after the previous one is a frame shown for 33.3 ms: judder), and the seconds in which the guest did not swap at all (a freeze).

usage: swap_judder_report.py <run dir or rcomp_title.err> [...] [--slices 2-30,31-140,140-300,300-410]

The slices are seconds of the frame-rate counter (not of the scripted scene clock): the first ~30 s are the game's start screen at a locked 60 fps; in the runs of waves 5 and 6 the opening
credits cinematic is around 140-150 s and the gameplay at the docks from about 150 s on. The "vb" field of the line is the histogram of vblanks between two swaps in that second.
"""
import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(r"RCOMP-FPS fps=([\d.]+) frame_ms=([\d.]+) swaps=(\d+).*? vb=([0-9:,\-]+)")


def parse(path):
    rows = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = LINE.match(line)
        if not m:
            continue
        hist = {}
        if m.group(4) != "-":
            for pair in m.group(4).split(","):
                k, v = pair.split(":")
                hist[int(k)] = int(v)
        rows.append((float(m.group(1)), hist))
    return rows


def main():
    p = argparse.ArgumentParser()
    p.add_argument("runs", nargs="+")
    p.add_argument("--slices", default="2-30,31-140,140-300,300-410")
    args = p.parse_args()
    slices = [tuple(int(x) for x in s.split("-")) for s in args.slices.split(",")]
    print("run | slice | swaps | 1 vblank | 2 vblanks | 3+ vblanks | seconds without a swap | seconds below 20 fps")
    print("--- | --- | ---: | ---: | ---: | ---: | ---: | ---:")
    for run in args.runs:
        path = Path(run)
        if path.is_dir():
            path = path / "rcomp_title.err"
        rows = parse(path)
        for a, b in slices:
            part = rows[a:b]
            total = sum(sum(h.values()) for _f, h in part)
            one = sum(h.get(0, 0) + h.get(1, 0) for _f, h in part)
            two = sum(h.get(2, 0) for _f, h in part)
            more = sum(v for _f, h in part for k, v in h.items() if k >= 3)
            frozen = sum(1 for f, _h in part if f == 0.0)
            slow = sum(1 for f, _h in part if f < 20.0)
            den = max(total, 1)
            print(f"{path.parent.name} | {a}-{b} s | {total} | {100 * one / den:.1f} % | {100 * two / den:.1f} % | {100 * more / den:.2f} % | {frozen} | {slow}")


if __name__ == "__main__":
    sys.exit(main())
