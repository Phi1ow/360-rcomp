#!/usr/bin/env python3
"""Measure observed swap pacing and select matching PC samples from title logs."""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


def analyze(raw, last_seconds):
    text = raw.decode("utf-8", errors="replace").replace("\x00", "")
    swaps = [(int(m[1]), int(m[2]), m.end()) for m in re.finditer(
        r"RCOMP-VD swap #(\d+) at (\d+) ms", text)]
    if len(swaps) < 2:
        raise ValueError("fewer than two swap observations; pacing NOT TESTED")
    if any(b[0] <= a[0] or b[1] <= a[1] for a, b in zip(swaps, swaps[1:])):
        raise ValueError("non-monotonic swaps; separate runs before analysis")
    start_ms = swaps[-1][1] - last_seconds * 1000
    selected = [s for s in swaps if s[1] >= start_ms]
    if len(selected) < 2:
        raise ValueError("requested window contains fewer than two observations")
    first, last = selected[0], selected[-1]
    intervals = [{"from_swap": a[0], "to_swap": b[0],
                  "elapsed_ms": b[1] - a[1],
                  "fps": 1000 * (b[0] - a[0]) / (b[1] - a[1])}
                 for a, b in zip(selected, selected[1:])]
    window_text = text[first[2]:last[2]]
    # Per-second frame-rate counter (title built with RCOMP_M6_FPS_COUNTER): the lines
    # whose cumulative guest swap number lies inside the measured window.
    counter_rows = [(float(m[1]), float(m[3]) if m[3] else -1.0, m[4] or "")
                    for m in re.finditer(r"RCOMP-FPS fps=([0-9.]+) frame_ms=[0-9.]+ swaps=(\d+)(?: cpu_cores=(-?[0-9.]+))?"
                                         r"(?: vb=([0-9:,-]+))?", text)
                    if first[0] <= int(m[2]) <= last[0]]
    per_second = [row[0] for row in counter_rows]
    cores = sorted(row[1] for row in counter_rows if row[1] >= 0)
    # Swaps by the vblanks since the previous swap ("vb=2:30" = 30 swaps two vblanks after their predecessor).
    pacing_seconds = [{int(k): int(v) for k, v in (item.split(":") for item in row[2].split(",") if ":" in item)}
                      for row in counter_rows if row[2]]
    # Include PC dump after the last swap up to the end of the collected log.
    pc_lines = [line for line in text[first[2]:].splitlines()
                if "RCOMP-PC " in line or "RCOMP-PC-CANDIDATE " in line]
    # Preserve the relocated RX contract emitted before the measurement window.
    formats = [line for line in text[:first[2]].splitlines()
               if "RCOMP-PC-FORMAT " in line]
    if formats:
        pc_lines.insert(0, formats[-1])
    stats = {}
    for key in ("draws", "draw_ms", "pipelines", "pipeline_ms", "fence_waits",
                "fence_wait_ms", "queue_submits", "submit_ms", "cp_interrupts",
                "interrupt_cb_ms"):
        values = [int(v) for v in re.findall(r"\b" + key + r"=(\d+)", window_text)]
        if len(values) > 1:
            stats[key] = values[-1] - values[0]
    counter = None
    if per_second:
        ordered = sorted(per_second)
        pick = lambda q: ordered[min(len(ordered) - 1, int(q * (len(ordered) - 1) + 0.5))]
        counter = {"seconds": len(ordered), "mean": sum(ordered) / len(ordered), "min": ordered[0],
                   "p5": pick(0.05), "median": pick(0.5), "p95": pick(0.95), "max": ordered[-1]}
        if cores:
            counter["cpu_cores_mean"] = sum(cores) / len(cores)
            counter["cpu_cores_median"] = cores[len(cores) // 2]
            counter["cpu_cores_max"] = cores[-1]
        if pacing_seconds:
            # Window totals, and how many seconds had (nearly) every swap at one vblank distance: the
            # title alternates between seconds locked at two vblanks (30 fps) and mixed ones (~36 fps).
            totals = {}
            locked = {}
            for second in pacing_seconds:
                swaps_in_second = sum(second.values())
                for k, n in second.items():
                    totals[k] = totals.get(k, 0) + n
                if swaps_in_second:
                    k, n = max(second.items(), key=lambda item: item[1])
                    if n >= 0.95 * swaps_in_second:
                        locked[k] = locked.get(k, 0) + 1
            counter["swap_vblank_intervals"] = {str(k): totals[k] for k in sorted(totals)}
            counter["seconds_locked_to_vblanks"] = {str(k): locked[k] for k in sorted(locked)}
            counter["seconds_not_locked"] = len(pacing_seconds) - sum(locked.values())
    if stats.get("draws"):
        stats["mean_draw_us"] = 1000 * stats.get("draw_ms", 0) / stats["draws"]
    if stats.get("cp_interrupts"):
        stats["mean_interrupt_us"] = 1000 * stats.get("interrupt_cb_ms", 0) / stats["cp_interrupts"]
    result = {
        "status": "PASS", "scope": "observed swap pacing, gameplay and resolution require visual evidence",
        "log_sha256": hashlib.sha256(raw).hexdigest(),
        "first_swap": first[0], "last_swap": last[0],
        "first_time_ms": first[1], "last_time_ms": last[1],
        "duration_s": (last[1] - first[1]) / 1000,
        "frames": last[0] - first[0],
        "fps": 1000 * (last[0] - first[0]) / (last[1] - first[1]),
        "interval_fps_min": min(i["fps"] for i in intervals),
        "interval_fps_max": max(i["fps"] for i in intervals),
        "intervals": intervals, "profile_counter_deltas": stats,
        "fatal_count": text.count("RCOMP-FATAL"),
        "crash_count": text.count("RCOMP-CRASH signal="),
    }
    if counter:
        result["per_second_counter"] = counter
    return result, pc_lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--last-seconds", type=float, default=90)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--pc-out", type=Path)
    args = parser.parse_args()
    if args.last_seconds <= 0:
        parser.error("--last-seconds must be positive")
    raw = args.log.read_bytes()
    try:
        result, pc_lines = analyze(raw, args.last_seconds)
    except ValueError as error:
        unmeasured = "fewer than two" in str(error)
        result = {"status": "NOT TESTED" if unmeasured else "FAIL",
                  "scope": "observed swap pacing",
                  "reason": str(error),
                  "log_sha256": hashlib.sha256(raw).hexdigest(),
                  "source": str(args.log)}
        if args.out:
            args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        if args.pc_out:
            args.pc_out.write_text("", encoding="utf-8")
        print(result["status"] + " pacing: " + result["reason"])
        sys.exit(2 if unmeasured else 1)
    result["source"] = str(args.log)
    if args.out:
        args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if args.pc_out:
        args.pc_out.write_text("\n".join(pc_lines) + "\n", encoding="utf-8")
    print(f"PASS pacing: {result['fps']:.2f} fps over {result['duration_s']:.2f}s "
          f"({result['frames']} swaps), intervals "
          f"{result['interval_fps_min']:.2f}..{result['interval_fps_max']:.2f} fps")
    print(json.dumps(result["profile_counter_deltas"], sort_keys=True))


if __name__ == "__main__":
    main()
