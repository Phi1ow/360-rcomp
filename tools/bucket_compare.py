#!/usr/bin/env python3
"""Compare two arms of a PS5 A/B scene by scene, from the per-second frame-rate counter lines.

usage: bucket_compare.py --a <run-dir> [<run-dir> ...] --b <run-dir> [<run-dir> ...] [--step 1200]

Every run directory holds the `rcomp_title.err` of a title built with RCOMP_M6_FPS_COUNTER. The
counter prints one `RCOMP-FPS fps=.. swaps=N cpu_cores=..` line per second; N, the cumulative guest
swap number, identifies the scene position, so the runs are cut into buckets of --step swaps and
the mean rate (and the mean number of busy cores) of each bucket is compared between the arms.
A title built with RCOMP_RUNTIME_WAIT_STATS also prints `waits=ready:polls:sleeps` (the guest waits
of that second that were ready at once, polls that found nothing, and sleeps); when every second
of a bucket has it, a second table compares those counts per second.

Why this and not the rate of the final 90 s: the title's rate is a staircase in the frame latency
(a swap comes one, two or more vblanks after the previous one), so a change of a few tenths of a
millisecond moves many frames across the 16.7 ms line in one scene and none in the next. The final
window only samples one or two scenes; the buckets show where a difference sits. A whole run is
needed: a run that ended early (the console was shut down, a crash) has no later buckets, and the
tool says so instead of inventing them.

Limit of the cut by swap number: it assumes the scenes sit at the same swap numbers in both arms.
They do not when the intro screen of the scripted run ends at another second (the autopilot presses
a key every 2 s, so a build that loads faster leaves the intro 2 s earlier and every later scene
moves by about 180 swaps). The buckets then compare different scenes and show the shift as a loss
or a gain of several per cent (wave 1, 1 October 2026: -5.5 % in the first bucket for a build whose
loading was faster). Use tools/scene_compare.py, which aligns on the end of the intro, whenever the
first scene change differs between the runs; this tool stays for the wait counts and for runs that
are known to be aligned.
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

LINE = re.compile(r"RCOMP-FPS fps=([0-9.]+) frame_ms=[0-9.]+ swaps=(\d+) cpu_cores=(-?[0-9.]+)"
                  r"(?: vb=\S*)?(?: waits=(\d+):(\d+):(\d+))?")


def mean_waits(items):
    """Mean (ready, polls, sleeps) per second over the items, or None when any item has no counts."""
    if not items or any(w is None for w in items):
        return None
    return tuple(statistics.mean(w[k] for w in items) for k in range(3))


def load(directory):
    """Rows (swaps, fps, cores, waits); waits is the (ready, polls, sleeps) of that second or None."""
    err = Path(directory) / "rcomp_title.err"
    text = err.read_text(encoding="utf-8", errors="replace")
    rows = [(int(m[2]), float(m[1]), float(m[3]), tuple(int(m[k]) for k in (4, 5, 6)) if m[4] else None)
            for m in LINE.finditer(text)]
    if not rows:
        raise SystemExit(f"{directory}: no RCOMP-FPS lines (title built without RCOMP_M6_FPS_COUNTER?)")
    return rows


def buckets(rows, step):
    """{bucket start: (mean fps, mean cores, seconds, mean waits)} for buckets with at least five seconds."""
    out = {}
    for start in range(0, rows[-1][0] + 1, step):
        chunk = [r for r in rows if start <= r[0] < start + step]
        if len(chunk) >= 5:
            out[start] = (statistics.mean(r[1] for r in chunk), statistics.mean(r[2] for r in chunk), len(chunk),
                          mean_waits([r[3] for r in chunk]))
    return out


def arm_means(runs, step):
    """{bucket: (mean fps, mean cores, number of runs having it, mean waits)} over the runs of an arm."""
    per_run = [buckets(load(d), step) for d in runs]
    result = {}
    for start in sorted(set().union(*[set(b) for b in per_run])):
        have = [b[start] for b in per_run if start in b]
        result[start] = (statistics.mean(x[0] for x in have), statistics.mean(x[1] for x in have), len(have),
                         mean_waits([x[3] for x in have]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--a", nargs="+", required=True, metavar="RUN_DIR")
    parser.add_argument("--b", nargs="+", required=True, metavar="RUN_DIR")
    parser.add_argument("--step", type=int, default=1200, help="swaps per bucket (default 1200, about 20-40 s)")
    args = parser.parse_args()
    a, b = arm_means(args.a, args.step), arm_means(args.b, args.step)
    print(f"{'swaps':>13} | {'A fps':>7} {'B fps':>7} {'B-A':>7} {'%':>6} | {'A cores':>7} {'B cores':>7} | runs A/B")
    for start in sorted(set(a) | set(b)):
        if start in a and start in b:
            (fa, ca, na, _), (fb, cb, nb, _) = a[start], b[start]
            print(f"{start:6d}-{start + args.step - 1:<6d} | {fa:7.2f} {fb:7.2f} {fb - fa:+7.2f} {100 * (fb / fa - 1):+5.1f}% | "
                  f"{ca:7.2f} {cb:7.2f} | {na}/{nb}")
        else:
            side = "A only" if start in a else "B only"
            print(f"{start:6d}-{start + args.step - 1:<6d} | {side}: this bucket exists in one arm only (a run ended early?)")
    if any(entry[3] for entry in list(a.values()) + list(b.values())):
        print()
        print("Guest waits per second, in thousands (ready at once : polls that timed out : sleeps); '-' = no counts")
        print(f"{'swaps':>13} | {'A ready':>8} {'A polls':>8} {'A sleep':>8} | {'B ready':>8} {'B polls':>8} {'B sleep':>8}")

        def cells(entry):
            if entry is None or entry[3] is None:
                return " ".join(f"{'-':>8}" for _ in range(3))
            return " ".join(f"{count / 1000:8.1f}" for count in entry[3])

        for start in sorted(set(a) | set(b)):
            print(f"{start:6d}-{start + args.step - 1:<6d} | {cells(a.get(start))} | {cells(b.get(start))}")


if __name__ == "__main__":
    main()
