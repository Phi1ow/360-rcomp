#!/usr/bin/env python3
"""Analyse the pass accounting of a profile build of the Xenos backend (RCOMP_XENOS_PROFILE_TIMINGS=ON).

The title log (rcomp_title.err) of such a build carries two kinds of lines, both written by the
command processor thread (see gpu/xenos/rexglue/README.md, "Pass accounting"):

  RCOMP-PASS-TABLE wall_ms=... swaps=... classes=...
  RCOMP-PASS key=<hex> n=... ticks=... px=... tiles=... <description>
      cumulative GPU time per pass class (a render target ownership transfer of one kind, a resolve
      dump, a resolve copy, ...) every 20 s of wall time; ticks are GPU timestamp ticks (100 MHz)

  RCOMP-EV D+ / D- / T / C / R ...
      the draw runs, transfers, clears and resolves of a few consecutive guest frames at several
      points of the run, in command order

Subcommands:
  passes <err> [--seconds A B | --from-wall S --to-wall S]   cost per frame of every pass class between two tables
  budget <err> --seconds A B | --swaps A B     GPU and command processor time per frame (RCOMP-PROFT / RCOMP-GPUT), counters of the
                                               shared memory uploads and register writes (slots 100 and up)
  cmdstat <err> --seconds A B | --swaps A B    the Vulkan commands replayed per frame by type, count and time (RCOMP-CMDSTAT)
  timeline <err> [--frame N]                   the event timeline of the window starting at (or containing) frame N
  windows <err A> <err B> [--windows N:F-T,...]  GPU time per frame of two runs in the same windows of counter seconds
  overwrites <err>                             the rectangle draws that overwrite whole render targets (depth and stencil, color), what
                                               the estimator said about them and what the transfers into those targets covered

Host validation is never PS5 evidence: the log must come from a PS5 run.
"""
import argparse
import re
import sys
from collections import OrderedDict, defaultdict

TICKS_PER_MS = 100_000  # 100 MHz GPU timestamp clock

# Names of the command processor slots of RCOMP-PROFT (pairs of count and TSC Mcycles, see rcomp_xenos/diagnostics.h).
PROFT_SLOTS = {0: "draw", 2: "pipeline", 4: "interrupt", 6: "fence_wait", 8: "submit", 10: "cp_idle", 12: "invalidate",
               14: "upload", 16: "tex_load", 20: "primary", 22: "swap", 24: "wait_reg_mem", 26: "queue+submit",
               32: "  rt_update", 34: "    transfers", 36: "resolve(copy)", 38: "  bindings", 40: "  textures",
               42: "  pipeline_cfg", 44: "  primitives", 46: "  modif+samplers", 48: "  buffers..draw",
               50: "  before loop", 52: "  host viewport", 54: "  dynamic state", 56: "  system constants",
               58: "await_replay", 60: "replay execute (replay thread if async)", 62: "(replay thread) end+submit",
               64: "    tex base request*", 66: "    tex 3D->2D views*", 68: "    tex usage transitions*",
               70: "    constants upload*", 72: "    tex descriptor snapshot*", 74: "    descriptor write*",
               76: "    descriptor bind*", 78: "    rt cache base update*", 80: "    rt post (pass, fb, barriers)*",
               82: "    vertex buffer residency*", 84: "    shm use + barriers + pass*", 86: "    draw commands*",
               88: "    shader modifications*", 90: "    samplers*",
               92: "    tex request: binding loop*", 94: "    tex request: shared memory ranges*",
               96: "    tex request: load commit*", 98: "    tex request: bindings update*"}
# Slots 64 to 99 are measured on one draw in eight: their counts and cycles are multiplied by this.
FINE_SLOT_FACTOR = 8
FINE_SLOT_LAST = 99
# Counter pairs (events, units) of the profile builds of the second part of wave 5: slots 100 and up (see rcomp_xenos/diagnostics.h).
COUNTER_SLOTS = {100: "upload vertex buffers (bytes)", 102: "upload index buffers (bytes)", 104: "upload textures (bytes)",
                 106: "upload other (bytes)", 108: "  same pages as the last upload, vertex (bytes)",
                 110: "  same pages as the last upload, index (bytes)", 112: "  same pages as the last upload, textures (bytes)",
                 114: "  same pages as the last upload, other (bytes)", 116: "draws; with no state register written since the last draw",
                 118: "fetch constant writes (dwords)", 120: "  unchanged fetch constant writes (dwords)",
                 122: "float constant writes (dwords)", 124: "  unchanged float constant writes (dwords)",
                 126: "single register writes; unchanged"}


def read_lines(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.readlines()


def fps_swaps(lines):
    """[(swaps after each counter second)] of the RCOMP-FPS lines, in order (index = seconds since the counter started)."""
    out = []
    for line in lines:
        match = re.match(r"RCOMP-FPS fps=[\d.]+ frame_ms=[\d.]+ swaps=(\d+)", line)
        if match:
            out.append(int(match.group(1)))
    return out


def swap_window(lines, seconds=None, swaps=None):
    """(first swap, last swap, seconds) of a window given on the counter clock (seconds) or directly in swaps."""
    series = fps_swaps(lines)
    if swaps is not None:
        return swaps[0], swaps[1], None
    if not series:
        raise SystemExit("no RCOMP-FPS lines: give the window in swaps")
    first, last = seconds
    last = min(int(last), len(series) - 1)
    return series[int(first)], series[last], last - int(first)


def parse_tables(lines):
    """Returns a list of tables: dict(wall_ms, swaps, classes={key: dict(n, ticks, px, tiles, desc)})."""
    tables = []
    current = None
    for line in lines:
        if line.startswith("RCOMP-PASS-TABLE"):
            fields = dict(re.findall(r"(\w+)=(\d+)", line))
            current = {"wall_ms": int(fields["wall_ms"]), "swaps": int(fields["swaps"]), "classes": OrderedDict(),
                       "cp": OrderedDict()}
            tables.append(current)
        elif line.startswith("RCOMP-CPCLASS ") and current is not None:
            match = re.match(r"RCOMP-CPCLASS key=([0-9a-f]+) draws=(\d+) cycles=(\d+) (.*)$", line.rstrip("\n"))
            if match:
                key, draws, cycles, desc = match.groups()
                current["cp"][int(key, 16)] = {"draws": int(draws), "cycles": int(cycles), "desc": desc}
        elif line.startswith("RCOMP-PASS ") and current is not None:
            match = re.match(r"RCOMP-PASS key=([0-9a-f]+) n=(\d+) ticks=(\d+) px=(\d+) tiles=(\d+) (.*)$", line.rstrip("\n"))
            if match:
                key, n, ticks, px, tiles, desc = match.groups()
                current["classes"][int(key, 16)] = {"n": int(n), "ticks": int(ticks), "px": int(px), "tiles": int(tiles), "desc": desc}
    return tables


def diff_tables(first, last):
    """Per-class difference of two cumulative tables (a class missing from `first` started at zero)."""
    frames = last["swaps"] - first["swaps"]
    rows = []
    for key, cls in last["classes"].items():
        before = first["classes"].get(key, {"n": 0, "ticks": 0, "px": 0, "tiles": 0})
        rows.append({"key": key, "desc": cls["desc"], "n": cls["n"] - before["n"], "ticks": cls["ticks"] - before["ticks"],
                     "px": cls["px"] - before["px"], "tiles": cls["tiles"] - before["tiles"]})
    return frames, rows


def kind_of(desc):
    return desc.split(" ", 1)[0]


def nearest(rows, swaps, key=lambda row: row["swaps"]):
    return min(rows, key=lambda row: abs(key(row) - swaps))


def seconds_between(series, first, last):
    """Counter seconds between two swap counts (the RCOMP-FPS lines carry the swap count of every second)."""
    def index(swaps):
        return min(range(len(series)), key=lambda i: abs(series[i] - swaps))
    return index(last) - index(first)


def cmd_passes(args):
    lines = read_lines(args.err)
    tables = parse_tables(lines)
    if len(tables) < 2:
        raise SystemExit("fewer than two RCOMP-PASS-TABLE blocks in the log")
    if args.seconds:
        first_swaps, last_swaps, _ = swap_window(lines, seconds=args.seconds)
        first, last = nearest(tables, first_swaps), nearest(tables, last_swaps)
    else:
        lo = [t for t in tables if t["wall_ms"] >= args.from_wall * 1000]
        hi = [t for t in tables if t["wall_ms"] <= args.to_wall * 1000]
        first = lo[0] if lo else tables[0]
        last = hi[-1] if hi else tables[-1]
    if last["swaps"] <= first["swaps"]:
        raise SystemExit("the selected tables span no frame")
    frames, rows = diff_tables(first, last)
    total = sum(r["ticks"] for r in rows)
    print(f"tables: wall {first['wall_ms'] / 1000:.0f} s (swaps {first['swaps']}) -> {last['wall_ms'] / 1000:.0f} s (swaps {last['swaps']}): {frames} frames")
    by_kind = defaultdict(lambda: [0, 0])
    for r in rows:
        by_kind[kind_of(r["desc"])][0] += r["ticks"]
        by_kind[kind_of(r["desc"])][1] += r["n"]
    print()
    print("per kind (GPU ms per frame, passes per frame):")
    for kind, (ticks, n) in sorted(by_kind.items(), key=lambda kv: -kv[1][0]):
        print(f"  {kind:18s} {ticks / TICKS_PER_MS / frames:7.3f} ms  {n / frames:7.2f}")
    print(f"  {'total':18s} {total / TICKS_PER_MS / frames:7.3f} ms")
    print()
    print("per class (GPU ms per frame, passes per frame, mean us per pass, host Mpixels or samples per frame):")
    for r in sorted(rows, key=lambda r: -r["ticks"])[: args.top]:
        if r["n"] <= 0:
            continue
        print(f"  {r['ticks'] / TICKS_PER_MS / frames:7.3f} ms  {r['n'] / frames:7.2f}  {r['ticks'] / TICKS_PER_MS * 1000 / r['n']:8.1f} us  "
              f"{r['px'] / frames / 1e6:8.2f}  {r['desc']}")


def cmd_drawclasses(args):
    """GPU and command processor time of the draw runs, per class (render targets plus coarse state) and per render
    target set: the phases of the frame (shadow maps, G-buffer, lighting, post-processing) with their share of both walls."""
    lines = read_lines(args.err)
    tables = parse_tables(lines)
    if len(tables) < 2 or not any(t["cp"] for t in tables):
        raise SystemExit("no RCOMP-CPCLASS lines: the log must come from a profile build with the draw run classes")
    first_swaps, last_swaps, seconds = swap_window(lines, seconds=args.seconds)
    first, last = nearest(tables, first_swaps), nearest(tables, last_swaps)
    frames = last["swaps"] - first["swaps"]
    if frames <= 0:
        raise SystemExit("the selected tables span no frame")
    # TSC cycles per counter second, from the RCOMP-PROFT wall counter (millions of TSC cycles) of the two nearest rows
    rows = parse_budget(lines)
    a, b = nearest(rows, first["swaps"], key=lambda r: r[0]), nearest(rows, last["swaps"], key=lambda r: r[0])
    if b[0] <= a[0]:
        raise SystemExit("no RCOMP-PROFT rows in the window")
    seconds_between_rows = seconds_between(fps_swaps(lines), a[0], b[0]) or 1
    cycles_per_second = (b[1] - a[1]) * 1e6 / seconds_between_rows
    classes = {}
    for key, cls in last["classes"].items():
        if not cls["desc"].startswith("draw-run"):
            continue
        before = first["classes"].get(key, {"n": 0, "ticks": 0, "px": 0, "tiles": 0})
        entry = classes.setdefault(key, {"desc": cls["desc"], "gpu": 0, "verts": 0, "draws": 0, "cp": 0, "cp_draws": 0})
        entry["gpu"] += cls["ticks"] - before["ticks"]
        entry["verts"] += cls["px"] - before["px"]
        entry["draws"] += cls["tiles"] - before["tiles"]
    for key, cls in last["cp"].items():
        before = first["cp"].get(key, {"draws": 0, "cycles": 0})
        entry = classes.setdefault(key, {"desc": cls["desc"], "gpu": 0, "verts": 0, "draws": 0, "cp": 0, "cp_draws": 0})
        entry["cp"] += cls["cycles"] - before["cycles"]
        entry["cp_draws"] += cls["draws"] - before["draws"]

    def show(title, entries):
        total_gpu = sum(e["gpu"] for e in entries.values()) / TICKS_PER_MS / frames
        total_cp = sum(e["cp"] for e in entries.values()) / cycles_per_second * 1000 / frames
        total_draws = sum(e["cp_draws"] for e in entries.values()) / frames
        print(f"{title}: GPU {total_gpu:.3f} ms, CP {total_cp:.3f} ms, {total_draws:.0f} draws per frame")
        print(f"  {'GPU ms':>7} {'CP ms':>7} {'draws':>7} {'kverts':>7} {'GPU us/d':>8} {'CP us/d':>8}  class")
        for e in sorted(entries.values(), key=lambda e: -(e["gpu"] / TICKS_PER_MS + e["cp"] / cycles_per_second * 1000))[: args.top]:
            gpu_ms = e["gpu"] / TICKS_PER_MS / frames
            cp_ms = e["cp"] / cycles_per_second * 1000 / frames
            draws = e["cp_draws"] / frames
            if gpu_ms < 0.02 and cp_ms < 0.02:
                continue
            per_gpu = e["gpu"] / TICKS_PER_MS * 1000 / e["draws"] if e["draws"] else 0.0
            per_cp = e["cp"] / cycles_per_second * 1e6 / e["cp_draws"] if e["cp_draws"] else 0.0
            print(f"  {gpu_ms:7.3f} {cp_ms:7.3f} {draws:7.1f} {e['verts'] / frames / 1000:7.1f} {per_gpu:8.2f} {per_cp:8.2f}  {e['desc']}")

    print(f"tables: swaps {first['swaps']} -> {last['swaps']} ({frames} frames, {seconds} counter seconds)")
    show("draw runs by class", classes)
    print()
    # the same without the state bits: the render target set alone
    sets = {}
    for e in classes.values():
        name = re.sub(r" state=[0-9a-f]+", "", e["desc"])
        s = sets.setdefault(name, {"desc": name, "gpu": 0, "verts": 0, "draws": 0, "cp": 0, "cp_draws": 0})
        for field in ("gpu", "verts", "draws", "cp", "cp_draws"):
            s[field] += e[field]
    show("draw runs by render target set", sets)


def parse_budget(lines):
    """[(swaps, wall Mcycles, {slot: (count, Mcycles)}, {gpu label: ticks})] from the RCOMP-PROFT / RCOMP-GPUT pairs."""
    out = []
    current = None
    for line in lines:
        if line.startswith("RCOMP-PROFT"):
            wall = int(re.search(r"wall=(\d+)", line).group(1))
            slots = {int(a): (int(b), int(c)) for a, b, c in re.findall(r" (\d+):(\d+)/(\d+)", line)}
            current = [slots.get(22, (0, 0))[0], wall, slots, None]
            out.append(current)
        elif line.startswith("RCOMP-GPUT") and current is not None:
            current[3] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
    return [tuple(row) for row in out if row[3] is not None]


def parse_cmdstat(lines):
    """[(swaps, {command name: (count, Mcycles)})] from the RCOMP-CMDSTAT lines (cumulative)."""
    out = []
    for line in lines:
        if line.startswith("RCOMP-CMDSTAT"):
            swaps = int(re.search(r"swaps=(\d+)", line).group(1))
            out.append((swaps, {name: (int(count), int(cycles)) for name, count, cycles in re.findall(r" (\w+):(\d+)/(\d+)", line)}))
    return out


def cmd_cmdstat(args):
    lines = read_lines(args.err)
    rows = parse_cmdstat(lines)
    if len(rows) < 2:
        raise SystemExit("fewer than two RCOMP-CMDSTAT lines: the log must come from a profile build with the command statistics")
    first_swaps, last_swaps, seconds = swap_window(lines, seconds=args.seconds, swaps=args.swaps)
    first, last = nearest(rows, first_swaps, key=lambda r: r[0]), nearest(rows, last_swaps, key=lambda r: r[0])
    frames = last[0] - first[0]
    if frames <= 0:
        raise SystemExit("the selected window spans no frame")
    budget = parse_budget(lines)
    a, b = nearest(budget, first[0], key=lambda r: r[0]), nearest(budget, last[0], key=lambda r: r[0])
    if b[0] <= a[0]:
        raise SystemExit("no RCOMP-PROFT rows in the window")
    if seconds is None:
        seconds = seconds_between(fps_swaps(lines), first[0], last[0])
    cycles_per_second = (b[1] - a[1]) * 1e6 / (seconds_between(fps_swaps(lines), a[0], b[0]) or 1)
    print(f"window: swaps {first[0]} -> {last[0]} ({frames} frames)")
    print()
    print("replayed Vulkan commands per frame (count, ms; the profile build measures each command, which costs about 25 ns each):")
    total_count = 0
    total_ms = 0.0
    entries = []
    for name, (count, cycles) in last[1].items():
        count0, cycles0 = first[1].get(name, (0, 0))
        n = (count - count0) / frames
        ms = (cycles - cycles0) * 1e6 / cycles_per_second * 1000 / frames
        entries.append((ms, n, name))
        total_count += n
        total_ms += ms
    for ms, n, name in sorted(entries, reverse=True):
        print(f"  {name:22s} {n:9.1f} {ms:8.3f} ms  {1000 * ms / n if n else 0.0:7.3f} us each")
    print(f"  {'total':22s} {total_count:9.1f} {total_ms:8.3f} ms")


def cmd_budget(args):
    lines = read_lines(args.err)
    rows = parse_budget(lines)
    if len(rows) < 2:
        raise SystemExit("fewer than two RCOMP-PROFT/RCOMP-GPUT pairs in the log")
    first_swaps, last_swaps, seconds = swap_window(lines, seconds=args.seconds, swaps=args.swaps)
    first, last = nearest(rows, first_swaps, key=lambda r: r[0]), nearest(rows, last_swaps, key=lambda r: r[0])
    frames = last[0] - first[0]
    if frames <= 0:
        raise SystemExit("the selected window spans no frame")
    if seconds is None:
        seconds = seconds_between(fps_swaps(lines), first[0], last[0])
    wall = last[1] - first[1]
    print(f"window: swaps {first[0]} -> {last[0]} ({frames} frames, {seconds} s on the counter clock)")
    print()
    print("GPU (timestamp queries, ms per frame; profile builds only):")
    for name in ("draw", "transfer", "resolve", "swap", "texload", "gap"):
        print(f"  {name:10s} {(last[3][name] - first[3][name]) / TICKS_PER_MS / frames:7.3f}")
    busy = sum(last[3][n] - first[3][n] for n in ("draw", "transfer", "resolve", "swap", "texload")) / TICKS_PER_MS / frames
    print(f"  {'busy':10s} {busy:7.3f}   (+ gap {(last[3]['gap'] - first[3]['gap']) / TICKS_PER_MS / frames:.3f})")
    print()
    print("command processor thread (TSC scopes, ms per frame and calls per frame):")
    for slot, name in PROFT_SLOTS.items():
        a, b = first[2].get(slot, (0, 0)), last[2].get(slot, (0, 0))
        calls, cycles = b[0] - a[0], b[1] - a[1]
        if slot >= 64:
            calls, cycles = calls * FINE_SLOT_FACTOR, cycles * FINE_SLOT_FACTOR
        if calls or cycles:
            print(f"  {name:13s} {cycles / wall * seconds * 1000 / frames:7.3f} ms  {calls / frames:9.1f}")
    counters = []
    for slot, name in COUNTER_SLOTS.items():
        a, b = first[2].get(slot, (0, 0)), last[2].get(slot, (0, 0))
        events, units = b[0] - a[0], b[1] - a[1]
        if events or units:
            counters.append((slot, name, events / frames, units / frames))
    if counters:
        print()
        print("counters (per frame: events, units):")
        for slot, name, events, units in counters:
            print(f"  {name:52s} {events:11.1f} {units:14.1f}")


GPU_CATEGORIES = ("draw", "transfer", "resolve", "texload")
# Windows of counter seconds of the GTA IV scenario of the 420 s runs (the scene windows of tools/scene_compare.py
# shifted by the end of the intro screen at 31-32 s); both runs are read at the same wall-clock seconds.
DEFAULT_WINDOWS = "D:65-138,E:150-182,F:195-285,G:294-345,H:354-400"


def gpu_window(lines, rows, first_second, last_second):
    """GPU categories in ms per frame and the frame rate between two counter seconds, or None if the log does not span them."""
    series = fps_swaps(lines)
    if last_second >= len(series):
        return None
    first = nearest(rows, series[first_second], key=lambda r: r[0])
    last = nearest(rows, series[last_second], key=lambda r: r[0])
    frames = last[0] - first[0]
    if frames <= 0:
        return None
    out = {name: (last[3][name] - first[3][name]) / TICKS_PER_MS / frames for name in GPU_CATEGORIES}
    out["busy"] = sum(out.values()) + (last[3]["swap"] - first[3]["swap"]) / TICKS_PER_MS / frames
    out["gap"] = (last[3]["gap"] - first[3]["gap"]) / TICKS_PER_MS / frames
    out["fps"] = frames / (last_second - first_second)
    return out


def parse_second_windows(text):
    windows = []
    for item in text.split(","):
        name, _, span = item.partition(":")
        start, _, end = span.partition("-")
        windows.append((name, int(start), int(end)))
    return windows


def cmd_windows(args):
    runs = []
    for path in (args.err_a, args.err_b):
        lines = read_lines(path)
        rows = parse_budget(lines)
        if len(rows) < 2:
            raise SystemExit(f"{path}: fewer than two RCOMP-PROFT/RCOMP-GPUT pairs in the log")
        runs.append((lines, rows))
    print(f"GPU ms per frame on the same counter seconds: A = {args.err_a}, B = {args.err_b}")
    print(f"{'window':>8} {'':>4} {'fps':>6} {'draw':>6} {'transf':>6} {'resolv':>6} {'texld':>6} {'gap':>5} {'busy':>6}")
    columns = ("fps", "draw", "transfer", "resolve", "texload", "gap", "busy")
    for name, first, last in parse_second_windows(args.windows):
        a, b = (gpu_window(lines, rows, first, last) for lines, rows in runs)
        if a is None or b is None:
            print(f"{name:>8} counter seconds {first}-{last}: not spanned by both runs")
            continue
        for tag, values in (("A", a), ("B", b)):
            print(f"{name:>8} {tag:>4} " + " ".join(f"{values[c]:{6 if c != 'gap' else 5}.2f}" for c in columns))
        print(f"{name:>8} {'B-A':>4} " + " ".join(f"{b[c] - a[c]:+{6 if c != 'gap' else 5}.2f}" for c in columns))


EVENT = re.compile(r"^RCOMP-EV (D\+|D-|T|C|R) (.*)$")


def parse_events(lines):
    """Returns a list of dict(kind, fields...) in log order; `n` is the sequence number."""
    events = []
    for line in lines:
        match = EVENT.match(line.rstrip("\n"))
        if not match:
            continue
        kind, rest = match.groups()
        event = {"kind": kind, "raw": rest}
        for name, value in re.findall(r"(\w+)=(\S*)", rest):
            event[name] = value
        events.append(event)
    return events


def windows(events):
    """Groups events into windows of consecutive frames (a gap of more than two frames starts a window)."""
    out, current, last_frame = [], [], None
    for event in events:
        frame = int(event["f"]) if "f" in event else last_frame
        if frame is not None and last_frame is not None and frame - last_frame > 2:
            out.append(current)
            current = []
        current.append(event)
        if frame is not None:
            last_frame = frame
    if current:
        out.append(current)
    return out


def cmd_timeline(args):
    events = parse_events(read_lines(args.err))
    groups = windows(events)
    print(f"{len(events)} events in {len(groups)} windows")
    for group in groups:
        frames = [int(e["f"]) for e in group if "f" in e]
        if args.frame is not None and not (min(frames) <= args.frame <= max(frames)):
            continue
        print(f"--- window frames {min(frames)}..{max(frames)}: {len(group)} events")
        for e in group:
            print(f"{e['kind']:2s} {e['raw']}")
        if args.frame is None:
            break


def parse_key(text):
    """b<base>.p<pitch>.m<msaa>.<D|C><format> -> dict, or None for '-'."""
    if text == "-":
        return None
    match = re.match(r"b(\d+)\.p(\d+)\.m(\d+)\.([DC])(\d+)$", text)
    if not match:
        return None
    base, pitch, msaa, kind, fmt = match.groups()
    return {"base": int(base), "pitch": int(pitch), "msaa": int(msaa), "depth": kind == "D", "fmt": int(fmt), "text": text}


def rectangle_area(text):
    """Pixels covered by 'x,y+WxH;x,y+WxH' (the r= field of a transfer event)."""
    total = 0
    for match in re.finditer(r"-?\d+,-?\d+\+(\d+)x(\d+)", text):
        total += int(match.group(1)) * int(match.group(2))
    return total


def cmd_overwrites(args):
    """Per window of the timeline: the rectangle list draws that overwrite entire render targets inside their rectangle
    (the `cand` mask), the verdict of the estimator and the area the transfers still cover."""
    events = parse_events(read_lines(args.err))
    if not events:
        raise SystemExit("no RCOMP-EV lines: the log must come from a profile build")
    totals = defaultdict(int)
    reasons = defaultdict(int)
    for group in windows(events):
        frames = [int(e["f"]) for e in group if "f" in e]
        runs = [e for e in group if e["kind"] == "D+" and "cand" in e]
        transfers = [e for e in group if e["kind"] == "T"]
        # cand is the mask of the render targets the draw overwrites entirely: bit 0 depth and stencil, 1 + i color i
        candidates = [e for e in runs if int(e["cand"]) != 0]
        depth_candidates = [e for e in candidates if int(e["cand"]) & 1]
        color_candidates = [e for e in candidates if int(e["cand"]) & 0b11110]
        estimated = [e for e in candidates if e.get("est") == "1"]
        overwritten = [e for e in candidates if "ow" in e]
        for e in candidates:
            if e.get("est") != "1":
                reasons[e.get("why", "?")] += 1
        tiles = 0
        for e in transfers:
            first, _, last = e["t"].partition("..")
            tiles += int(last) - int(first)
        area = sum(rectangle_area(e.get("r", "")) for e in transfers)
        print(f"window frames {min(frames)}..{max(frames)}: rectangle draws {len(runs)}, "
              f"candidates {len(candidates)} (depth {len(depth_candidates)}, color {len(color_candidates)}), "
              f"estimated {len(estimated)}, overwriting {len(overwritten)} | "
              f"transfers {len(transfers)}, {tiles} tiles, {area} pixels")
        totals["runs"] += len(runs)
        totals["candidates"] += len(candidates)
        totals["estimated"] += len(estimated)
        totals["transfers"] += len(transfers)
        totals["tiles"] += tiles
        totals["area"] += area
    print(f"total: rectangle draws {totals['runs']}, candidates {totals['candidates']}, estimated {totals['estimated']}, "
          f"transfers {totals['transfers']} over {totals['tiles']} tiles / {totals['area']} pixels")
    if reasons:
        print("candidates the estimator refused, by reason code (see draw_extent_estimator.h): " +
              ", ".join(f"why={k}: {v}" for k, v in sorted(reasons.items())))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    passes = sub.add_parser("passes")
    passes.add_argument("err")
    passes.add_argument("--from-wall", type=float, default=0.0, help="first table at or after this wall time (s)")
    passes.add_argument("--to-wall", type=float, default=1e9, help="last table at or before this wall time (s)")
    passes.add_argument("--seconds", type=int, nargs=2, metavar=("FROM", "TO"),
                        help="window on the RCOMP-FPS counter clock (overrides --from-wall/--to-wall)")
    passes.add_argument("--top", type=int, default=30)
    passes.set_defaults(func=cmd_passes)
    budget = sub.add_parser("budget")
    budget.add_argument("err")
    group = budget.add_mutually_exclusive_group(required=True)
    group.add_argument("--seconds", type=int, nargs=2, metavar=("FROM", "TO"), help="window on the RCOMP-FPS counter clock")
    group.add_argument("--swaps", type=int, nargs=2, metavar=("FROM", "TO"), help="window in guest swaps")
    budget.set_defaults(func=cmd_budget)
    windows_parser = sub.add_parser("windows")
    windows_parser.add_argument("err_a")
    windows_parser.add_argument("err_b")
    windows_parser.add_argument("--windows", default=DEFAULT_WINDOWS, metavar="NAME:FROM-TO,...",
                                help="windows in counter seconds (default: the GTA IV scenario, windows D to H)")
    windows_parser.set_defaults(func=cmd_windows)
    timeline = sub.add_parser("timeline")
    timeline.add_argument("err")
    timeline.add_argument("--frame", type=int, default=None)
    timeline.set_defaults(func=cmd_timeline)
    overwrites = sub.add_parser("overwrites")
    overwrites.add_argument("err")
    overwrites.set_defaults(func=cmd_overwrites)
    drawclasses = sub.add_parser("drawclasses")
    drawclasses.add_argument("err")
    drawclasses.add_argument("--seconds", type=int, nargs=2, metavar=("FROM", "TO"), required=True,
                             help="window on the RCOMP-FPS counter clock")
    drawclasses.add_argument("--top", type=int, default=30)
    drawclasses.set_defaults(func=cmd_drawclasses)
    cmdstat = sub.add_parser("cmdstat")
    cmdstat.add_argument("err")
    group = cmdstat.add_mutually_exclusive_group(required=True)
    group.add_argument("--seconds", type=int, nargs=2, metavar=("FROM", "TO"), help="window on the RCOMP-FPS counter clock")
    group.add_argument("--swaps", type=int, nargs=2, metavar=("FROM", "TO"), help="window in guest swaps")
    cmdstat.set_defaults(func=cmd_cmdstat)
    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
