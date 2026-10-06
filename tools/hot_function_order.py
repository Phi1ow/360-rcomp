#!/usr/bin/env python3
"""Linker symbol-ordering file (lld --symbol-ordering-file) from PC-sampler runs: the recompiled functions the guest threads
actually run, hottest first, so the hot code of a title that is 75-95 MB of flat-profile text sits together (fewer iTLB and
instruction-cache misses). A layout change only: no instruction of any function changes.

usage: hot_function_order.py --run <pc-window.log>:<title.map>[:<group>=<weight>,...] ... --out hot_order.txt [--top 4000]
Each --run is one sampler log with the link map of the build that produced it (function names are address-based, so the order
carries over to every build of the same game inventory). Groups default to main=1.0 and others=0.5 (gpu is not guest code).
"""
import argparse
import bisect
import collections
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pc_ppc_profile as ppp  # noqa: E402  (parse_log, load_bias_from_map)

LINKAGE_SUB = re.compile(r'^sub_([0-9A-Fa-f]{8})\(')
LINKAGE_IMP = re.compile(r'^(__imp__[A-Za-z0-9_]+)$')
LINKAGE_HELPER = re.compile(r'^(__(?:save|rest)(?:gprlr|fpr|vmx|gpr|fprlr|vmx128)?_?\w*)\(PPCContext&')


def linkage_name(name):
    """The extern "C" symbol of a recompiled function from the link map name, or None for a runtime function."""
    m = LINKAGE_IMP.match(name)
    if m:
        return m.group(1)
    m = LINKAGE_SUB.match(name)
    if m:
        return '__imp__sub_' + m.group(1).upper()
    m = LINKAGE_HELPER.match(name)
    if m:
        return '__imp__' + m.group(1)
    return None


def read_map(map_path):
    """[(address, size, name)] sorted: the symbols of the link map with a size (function-like entries)."""
    out = []
    for line in open(map_path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+)\s+(.*)$', line)
        if not m:
            continue
        va, size, name = int(m[1], 16), int(m[3], 16), m[5].strip()
        if size and not name.startswith(('/', '<', '.')):
            out.append((va, size, name))
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', action='append', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--top', type=int, default=4000)
    a = ap.parse_args()
    heat = collections.Counter()
    for spec in a.run:
        parts = spec.split(':')
        log, mp = parts[0], parts[1]
        weights = {'main': 1.0, 'others': 0.5}
        if len(parts) > 2 and parts[2]:
            weights = {k: float(v) for k, v in (kv.split('=') for kv in parts[2].split(','))}
        syms = read_map(mp)
        keys = [s[0] for s in syms]
        for group, weight in weights.items():
            counts, rx = ppp.parse_log(log, group)
            bias = ppp.load_bias_from_map(mp, rx)
            if bias is None or not counts:
                print(f'warning: no usable samples for group {group} of {log}', file=sys.stderr)
                continue
            total = float(sum(counts.values()))
            for rip, n in counts.items():
                if not (rx[0] <= rip < rx[1]):
                    continue
                addr = rip - bias
                i = bisect.bisect_right(keys, addr) - 1
                if i < 0 or addr >= syms[i][0] + syms[i][1]:
                    continue
                # several map entries share an address (the alias and the implementation): any of them names the function
                j = i
                name = None
                while j >= 0 and syms[j][0] == syms[i][0]:
                    name = linkage_name(syms[j][2]) or name
                    j -= 1
                if name:
                    heat[name] += weight * n / total
    ordered = [n for n, _ in heat.most_common(a.top)]
    Path(a.out).write_text('\n'.join(ordered) + '\n', encoding='utf-8', newline='\n')
    covered = sum(heat[n] for n in ordered) / (sum(heat.values()) or 1.0)
    print(f'{len(ordered)} functions written to {a.out} (they hold {100.0 * covered:.1f} % of the sampled guest-function weight; {len(heat)} functions seen)')


if __name__ == '__main__':
    sys.exit(main())
