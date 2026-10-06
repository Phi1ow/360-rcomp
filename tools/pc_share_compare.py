#!/usr/bin/env python3
"""Compare the PC-sample profiles (pc-symbols.txt) of two runs: which functions gained or lost share.

usage: pc_share_compare.py <before pc-symbols.txt> <after pc-symbols.txt>
           [--group main] [--top 12] [--match SUBSTRING ...]

pc-symbols.txt is written by build/prime-gta-resume-20260929/pcsym.py from the 1 kHz sampler of a
diagnostic (RCOMP_M6_PC_SAMPLER=ON) run. It has, per thread group, a section of the interrupted
instruction pointers ("== main raw total N", one "<share>% <function>" line each) and a section of
stack-scan candidates ("== main stack-scan heuristic ...", "<share>% of raw samples <function>"),
which name a caller found on the stack when the instruction pointer was inside a system library
(for example a time system call made by steady_clock::now()).

The tool prints the shares of the functions that moved most and of every function whose name
contains one of the --match substrings, in both sections. Shares are percentages of the group's raw
samples; a function that is absent from a file is reported as 0.0. Sampling noise between two runs
of the same binary is a few tenths of a percent; do not read smaller moves as effects.
"""
import argparse
import re
import sys
from pathlib import Path

ROW = re.compile(r"^\s*(?P<share>[0-9]+\.[0-9]+)% (?:of raw samples )?(?P<name>.+?)\s*$")


def parse(path):
    """{group: {'raw': {name: share}, 'stack': {name: share}}}"""
    groups = {}
    section = None
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("== "):
            head = re.match(r"^== (\S+) (raw total|stack-scan heuristic)", line)
            if not head:
                section = None
                continue
            section = groups.setdefault(head.group(1), {"raw": {}, "stack": {}})["raw" if head.group(2) == "raw total" else "stack"]
            continue
        row = ROW.match(line)
        if section is not None and row:
            # A function can appear under several addresses; shares add up.
            section[row.group("name")] = section.get(row.group("name"), 0.0) + float(row.group("share"))
    return groups


def table(title, before, after, top, matches):
    names = set(before) | set(after)
    moved = sorted(names, key=lambda n: abs(after.get(n, 0.0) - before.get(n, 0.0)), reverse=True)[:top]
    chosen = list(moved)
    for needle in matches:
        chosen += [n for n in sorted(names) if needle in n and n not in chosen]
    print(title)
    print(f"  {'before':>7} {'after':>7} {'delta':>7}  function")
    for name in chosen:
        b, a = before.get(name, 0.0), after.get(name, 0.0)
        print(f"  {b:6.1f}% {a:6.1f}% {a - b:+6.1f}%  {name[:110]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--group", default="main", help="thread group to compare (main, others)")
    parser.add_argument("--top", type=int, default=12, help="how many of the largest moves to list")
    parser.add_argument("--match", nargs="*", default=[], metavar="SUBSTRING", help="always list functions containing these")
    args = parser.parse_args()
    before, after = parse(args.before), parse(args.after)
    if args.group not in before or args.group not in after:
        sys.exit(f"group '{args.group}' missing (found: {sorted(before)} / {sorted(after)})")
    table(f"[{args.group}] interrupted instruction pointer", before[args.group]["raw"], after[args.group]["raw"], args.top, args.match)
    table(f"[{args.group}] stack-scan candidates (caller found on the stack)", before[args.group]["stack"],
          after[args.group]["stack"], args.top, args.match)


if __name__ == "__main__":
    main()
