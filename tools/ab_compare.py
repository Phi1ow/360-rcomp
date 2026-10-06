#!/usr/bin/env python3
"""Compare two arms of a PS5 A/B from the run directories of tools/analyze_frame_pacing.py.

usage: ab_compare.py --a <run-dir> [<run-dir> ...] --b <run-dir> [<run-dir> ...]

Every run directory holds the `summary.json` written by tools/analyze_frame_pacing.py (the
final measurement window of one 420 s run). The tool prints, per run, the rate, the interval
range and the statuses of `pause-monitor.json` and `timebase.json` when present; then, per
arm, the mean and the sample standard deviation; then B minus A with a 95 % interval from the
pooled spread (Student t, n_a + n_b - 2 degrees of freedom). The same comparison is repeated
on the intervals that every run has in common (same swap number), which removes the effect of
measurement windows that start at different points of the scene.

The title does not run at one rate: its one-second rates are locked at 30.0 fps (the game waits
two vblanks between swaps, 58 % of the lean-run seconds) or near 35.6 fps (GPU- and CPU-limited, 40 %),
and which regime a second falls in follows the scene. The rate of the final window therefore moves
with how many seconds of each regime it contains (a scripted event one second earlier or later
shifts it by about 0.07 fps), while only the ~36 fps rate measures capability: the locked seconds are
a game limit. Every run is thus also split at --threshold fps (default 33): the rate of the fast
seconds, the rate of the locked seconds and the share of time spent fast. Compare the fast rate
to judge a speed change and the share to see whether the window shifted.

The window also moves with the scripted intro: the autopilot presses a key every 2 s, so a build
that loads faster leaves the intro screen 2 s earlier and its last 90 s hold 2 s more of the final
(slower, 30 fps) scene: 0.4 fps lower with nothing slower in any scene (wave 1, 1 October 2026).
When the first scene change of the arms differs, compare scene by scene with tools/scene_compare.py.

Reading the result: an interval that contains zero means the difference is not measurable
with these runs. A single run of one binary can differ by about 1 % from its repeat
(phases 62 and 64), so use three or more runs per arm to claim 0.5 %. The tool reports
numbers only; it does not decide whether a configuration is acceptable.
"""
import argparse
import json
import math
import statistics
import sys
from pathlib import Path

# Two-sided 95 % Student t quantiles for 1..30 degrees of freedom.
T95 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
       2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
       2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]


def t95(df):
    return T95[df - 1] if 1 <= df <= len(T95) else 1.96


def load_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def regimes(intervals, threshold):
    """(fast rate, locked rate, share of time fast) of a run's one-second intervals."""
    def rate(items):
        millis = sum(i["elapsed_ms"] for i in items)
        return 1000.0 * sum(i["to_swap"] - i["from_swap"] for i in items) / millis if millis else None
    fast = [i for i in intervals if i["fps"] >= threshold]
    slow = [i for i in intervals if i["fps"] < threshold]
    total = sum(i["elapsed_ms"] for i in intervals)
    return rate(fast), rate(slow), (sum(i["elapsed_ms"] for i in fast) / total if total else 0.0)


def load_run(directory, threshold=33.0):
    d = Path(directory)
    summary = load_json(d / "summary.json")
    if not summary or summary.get("status") != "PASS" or "fps" not in summary:
        raise SystemExit(f"{d}: summary.json missing or not PASS (run analyze_frame_pacing.py first)")
    pause = load_json(d / "pause-monitor.json") or {}
    clock = load_json(d / "timebase.json") or {}
    fast_rate, locked_rate, fast_share = regimes(summary.get("intervals", []), threshold)
    return {
        "name": d.name,
        "fps": summary["fps"],
        "fast_rate": fast_rate,
        "locked_rate": locked_rate,
        "fast_share": fast_share,
        "min": summary.get("interval_fps_min"),
        "max": summary.get("interval_fps_max"),
        "fatal": summary.get("fatal_count", 0) + summary.get("crash_count", 0),
        "pause": pause.get("status", "NOT TESTED"),
        "clock": clock.get("status", "NOT TESTED"),
        "intervals": {i["from_swap"]: i for i in summary.get("intervals", [])},
    }


def mean_sd(values):
    return statistics.mean(values), (statistics.stdev(values) if len(values) > 1 else float("nan"))


def compare(label, a, b):
    """Print B minus A for two lists of rates; return nothing."""
    ma, sa = mean_sd(a)
    mb, sb = mean_sd(b)
    print(f"  {label}: A n={len(a)} mean {ma:.3f} sd {sa:.3f} | B n={len(b)} mean {mb:.3f} sd {sb:.3f}")
    diff = mb - ma
    df = len(a) + len(b) - 2
    if df < 1:
        print(f"  {label}: B - A = {diff:+.3f} fps ({100 * diff / ma:+.2f} %), no interval (one run per arm)")
        return
    # Pooled variance; an arm with one run contributes no spread of its own.
    ss = sum((len(v) - 1) * s * s for v, s in ((a, sa), (b, sb)) if len(v) > 1)
    pooled = math.sqrt(ss / df)
    half = t95(df) * pooled * math.sqrt(1 / len(a) + 1 / len(b))
    lo, hi = diff - half, diff + half
    verdict = "contains zero: not measurable" if lo <= 0 <= hi else "excludes zero"
    print(f"  {label}: B - A = {diff:+.3f} fps ({100 * diff / ma:+.2f} %), 95 % interval "
          f"[{lo:+.3f}, {hi:+.3f}] fps = [{100 * lo / ma:+.2f} %, {100 * hi / ma:+.2f} %] ({verdict})")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--a", nargs="+", required=True, metavar="RUN_DIR", help="runs of arm A (baseline)")
    parser.add_argument("--b", nargs="+", required=True, metavar="RUN_DIR", help="runs of arm B (change)")
    parser.add_argument("--threshold", type=float, default=33.0, metavar="FPS",
                        help="one-second rates at or above this are the fast regime (default 33)")
    args = parser.parse_args()
    arms = {"A": [load_run(d, args.threshold) for d in args.a], "B": [load_run(d, args.threshold) for d in args.b]}
    for arm, runs in arms.items():
        for r in runs:
            rng = f"{r['min']:.2f}-{r['max']:.2f}" if r["min"] is not None else "?"
            split = (f"fast {r['fast_rate']:.2f} ({100 * r['fast_share']:.0f} % of time) locked {r['locked_rate']:.2f}"
                     if r["fast_rate"] and r["locked_rate"] else "no regime split")
            print(f"{arm} {r['name']:<40} {r['fps']:8.3f} fps  intervals {rng:<12} {split} "
                  f"pause={r['pause']} clock={r['clock']} fatal+crash={r['fatal']}")
    print("Final window of each run:")
    compare("rate", [r["fps"] for r in arms["A"]], [r["fps"] for r in arms["B"]])
    everyone = arms["A"] + arms["B"]
    if all(r["fast_rate"] for r in everyone):
        compare("fast-regime rate", [r["fast_rate"] for r in arms["A"]], [r["fast_rate"] for r in arms["B"]])
        compare("share of time fast (%)", [100 * r["fast_share"] for r in arms["A"]], [100 * r["fast_share"] for r in arms["B"]])
    everyone = arms["A"] + arms["B"]
    common = set.intersection(*[set(r["intervals"]) for r in everyone])
    if common:
        def aligned(r):
            by = r["intervals"]
            swaps = sum(by[k]["to_swap"] - by[k]["from_swap"] for k in common)
            millis = sum(by[k]["elapsed_ms"] for k in common)
            return 1000.0 * swaps / millis
        print(f"Intervals common to all {len(everyone)} runs ({len(common)} of them, aligned by swap number):")
        compare("aligned rate", [aligned(r) for r in arms["A"]], [aligned(r) for r in arms["B"]])
    else:
        print("No interval is common to all runs; aligned comparison skipped.")
    bad = [r["name"] for r in everyone if r["pause"] != "PASS" or r["clock"] != "PASS" or r["fatal"]]
    if bad:
        print("WARNING: pause/clock not PASS or fatal/crash seen in: " + ", ".join(bad), file=sys.stderr)


if __name__ == "__main__":
    main()
