#!/usr/bin/env python3
"""Compare two arms of a PS5 A/B scene by scene, aligned on content instead of on the swap number.

usage: scene_compare.py --a <run-dir> [<run-dir> ...] --b <run-dir> [<run-dir> ...]
                        [--windows NAME:FROM-TO,...] [--t0-fps 50 | --t0 SECOND]

Every run directory holds the `rcomp_title.err` of a title built with RCOMP_M6_FPS_COUNTER (one
`RCOMP-FPS fps=.. swaps=N cpu_cores=..` line per second).

Why not tools/bucket_compare.py (cuts by swap number) or the final 90 s window of
tools/analyze_frame_pacing.py: the scripted run starts with an intro screen that holds 60 fps until
the game accepts the autopilot's next key press. The presses come every 2 s, so the screen ends
2 s earlier or later whenever loading is a little faster or slower (wave 1, 1 October 2026: 34 s
against 36-37 s for a build whose loading is faster). Everything after that sits at a different
swap number and a different second of the run; scene boundaries move by about 180 swaps. A swap
bucket or a final window then mixes different scenes in the two arms and reports the shift as a
gain or a loss of several per cent. Here the time zero of each run is the first second below
--t0-fps (the end of the intro screen), and each window is measured in seconds after it: the same
content in every run. The windows are interior stretches of the scenes of that scripted scenario
(trimmed away from the boundaries, which move by a second or two between runs); pass --windows to
cut another scenario.

When the first scene of an arm is itself above the threshold (a build that renders it at the 60 fps
lock) the detection finds a later second and the windows no longer hold the same content; the autopilot
is driven by the wall clock, so --t0 SECOND (the same counter second for every run, for example the
first scene change of the reference arm) compares the same wall-clock seconds instead.

The output has the second of the first scene change (a speed measure of loading), then for each
window the mean rate and the mean number of busy cores of every arm and B against A. A window a
run is too short to reach is skipped for that run. Differences smaller than the spread of
identical runs (about 0.3 %) mean nothing; the tool does not decide.
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

LINE = re.compile(r"RCOMP-FPS fps=([0-9.]+) frame_ms=[0-9.]+ swaps=(\d+) cpu_cores=(-?[0-9.]+)")

# The GTA IV scenario of the 420 s runs (seconds after the end of the intro screen), measured on
# phases 65-74: interior of each scene, boundaries excluded.
DEFAULT_WINDOWS = "42fps:3-8,37fps:13-20,40fps:24-27,D:33-106,E:118-150,F:163-253,G:262-313,H:322-368"


def load(directory):
    """[(fps, cores)] per second of one run."""
    err = Path(directory) / "rcomp_title.err"
    text = err.read_text(encoding="utf-8", errors="replace")
    rows = [(float(m[1]), float(m[3])) for m in LINE.finditer(text)]
    if not rows:
        raise SystemExit(f"{directory}: no RCOMP-FPS lines (title built without RCOMP_M6_FPS_COUNTER?)")
    return rows


def first_change(rows, threshold, skip=20, sustained=4):
    """Index (second) of the first line below `threshold` fps after the first `skip` lines, or None.

    The rate must stay below the threshold for `sustained` consecutive seconds: a one-second hitch in
    the intro screen (a 34 fps second between 60 fps ones was seen) is not the end of the intro."""
    for i in range(skip, len(rows) - sustained + 1):
        if all(rows[j][0] < threshold for j in range(i, i + sustained)):
            return i
    return None


def parse_windows(text):
    windows = []
    for item in text.split(","):
        name, _, span = item.partition(":")
        start, _, end = span.partition("-")
        windows.append((name, int(start), int(end)))
    return windows


def window_means(rows, t0, windows):
    """{name: (mean fps, mean cores)} for the windows the run reaches (at least four seconds of it)."""
    out = {}
    for name, start, end in windows:
        chunk = rows[t0 + start:t0 + end + 1]
        if len(chunk) >= 4 and t0 + end < len(rows):
            out[name] = (statistics.mean(r[0] for r in chunk), statistics.mean(r[1] for r in chunk))
    return out


def arm_summary(runs, threshold, windows, fixed_t0=None):
    """(list of t0 per run, {window: (mean fps, mean cores, sample sd of fps or None, runs)})."""
    t0s, per_run = [], []
    for directory in runs:
        rows = load(directory)
        t0 = fixed_t0 if fixed_t0 is not None else first_change(rows, threshold)
        if t0 is None:
            raise SystemExit(f"{directory}: the rate never drops below {threshold} fps after the first 20 s")
        t0s.append(t0)
        per_run.append(window_means(rows, t0, windows))
    result = {}
    for name, _, _ in windows:
        have = [r[name] for r in per_run if name in r]
        if have:
            fps = [h[0] for h in have]
            result[name] = (statistics.mean(fps), statistics.mean(h[1] for h in have),
                            statistics.stdev(fps) if len(fps) > 1 else None, len(have))
    return t0s, result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--a", nargs="+", required=True, metavar="RUN_DIR")
    parser.add_argument("--b", nargs="+", required=True, metavar="RUN_DIR")
    parser.add_argument("--windows", default=DEFAULT_WINDOWS, metavar="NAME:FROM-TO,...",
                        help="windows in seconds after the first scene change (default: the GTA IV scenario)")
    parser.add_argument("--t0-fps", type=float, default=50.0,
                        help="the first second below this rate is time zero (default 50)")
    parser.add_argument("--t0", type=int, default=None, metavar="SECOND",
                        help="use this counter second as time zero of every run instead of detecting it")
    args = parser.parse_args()
    windows = parse_windows(args.windows)
    t0_a, a = arm_summary(args.a, args.t0_fps, windows, args.t0)
    t0_b, b = arm_summary(args.b, args.t0_fps, windows, args.t0)

    print(f"first scene change (s):  A {t0_a}  mean {statistics.mean(t0_a):.1f}   |   B {t0_b}  mean {statistics.mean(t0_b):.1f}"
          f"   (B - A {statistics.mean(t0_b) - statistics.mean(t0_a):+.1f} s; the autopilot presses a key every 2 s)")
    print()
    print(f"{'window':>8} {'s after t0':>11} | {'A fps':>7} {'B fps':>7} {'B-A':>7} {'%':>6} | {'A cores':>7} {'B cores':>7} {'%':>6} | runs A/B")
    for name, start, end in windows:
        if name not in a or name not in b:
            side = "A" if name in a else ("B" if name in b else "neither arm")
            print(f"{name:>8} {f'{start}-{end}':>11} | only {side} reaches this window")
            continue
        (fa, ca, _, na), (fb, cb, _, nb) = a[name], b[name]
        print(f"{name:>8} {f'{start}-{end}':>11} | {fa:7.2f} {fb:7.2f} {fb - fa:+7.2f} {100 * (fb / fa - 1):+5.1f}% | "
              f"{ca:7.2f} {cb:7.2f} {100 * (cb / ca - 1):+5.1f}% | {na}/{nb}")
    spreads = [(n, a[n][2], b[n][2]) for n, _, _ in windows if n in a and n in b and (a[n][2] is not None or b[n][2] is not None)]
    if spreads:
        print()
        print("sample standard deviation of the window rate between the runs of an arm (fps):")
        for n, sa, sb in spreads:
            print(f"  {n:>8}: A {'-' if sa is None else f'{sa:.3f}'}   B {'-' if sb is None else f'{sb:.3f}'}")


if __name__ == "__main__":
    main()
