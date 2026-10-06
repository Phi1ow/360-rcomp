"""Symbolize interrupted RIPs and independent, unvalidated stack candidates."""
import bisect
import collections
import os
import re
import sys

mapped = []
text_range = None
map_path = os.environ.get('RCOMP_PC_MAP', 'build/prime-radv-resume-20260929/artifacts/game/title.map')
for line in open(map_path, encoding='utf-8', errors='replace'):
    m = re.match(r'\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+)\s+(.*)$', line)
    if not m:
        continue
    va, size, name = int(m[1], 16), int(m[3], 16), m[5].strip()
    if name == '.text':
        text_range = (va, va + size)
    elif size and not name.startswith(('/', '<', '.')):
        mapped.append((va, size, name))
mapped.sort()
keys = [r[0] for r in mapped]
lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
formats = [line for line in lines if 'RCOMP-PC-FORMAT ' in line]
version2 = bool(formats)
load_bias = None if version2 else 0x400000
rx = None
if version2:
    values = dict(re.findall(r'\b(rx_begin|rx_end|rx_valid)=([0-9a-f]+)', formats[-1]))
    if text_range and values.get('rx_valid') == '1' and 'rx_begin' in values and 'rx_end' in values:
        begin, end = int(values['rx_begin'], 16), int(values['rx_end'], 16)
        if begin < end and end - begin == text_range[1] - text_range[0]:
            rx = (begin, end)
            load_bias = begin - text_range[0]
if load_bias is not None and rx is None and text_range:
    rx = (text_range[0] + load_bias, text_range[1] + load_bias)

def sym(pc):
    legacy = bool(pc >> 63)
    if legacy:
        pc &= (1 << 63) - 1
    label = '[legacy stack heuristic; unvalidated] ' if legacy else ''
    if load_bias is None or rx is None or not rx[0] <= pc < rx[1]:
        return label + '?%x (outside verified title text)' % pc
    a = pc - load_bias
    i = bisect.bisect_right(keys, a) - 1
    if i >= 0 and a < mapped[i][0] + mapped[i][1]:
        return label + mapped[i][2][:100]
    return label + '?%x (title text)' % pc

limit = int(sys.argv[2]) if len(sys.argv) > 2 else 25
print('format', '2 interrupted-rip; candidates never added to raw' if version2
      else '1 historical; fixed load bias 0x400000; bit63 samples are stack heuristics')
if version2:
    print('relocated RX', 'unavailable; symbolization fails closed' if rx is None
          else '%x..%x load_bias=%x' % (*rx, load_bias))
for group in ('main', 'others', 'gpu'):
    raw, candidate = collections.Counter(), collections.Counter()
    raw_total = candidate_total = 0
    for line in lines:
        if f'RCOMP-PC {group} ' in line:
            counts, family = raw, 'raw'
        elif f'RCOMP-PC-CANDIDATE {group} ' in line:
            counts, family = candidate, 'candidate'
        else:
            continue
        total = re.search(r'\btotal=(\d+)', line)
        if not total:
            continue
        if family == 'raw':
            raw_total += int(total[1])
        else:
            candidate_total += int(total[1])
        for pc, n in re.findall(r' ([0-9a-f]+):(\d+)', line):
            counts[sym(int(pc, 16))] += int(n)
    print('==', group, 'raw total', raw_total)
    for name, n in raw.most_common(limit):
        print(f'{100*n/max(raw_total,1):5.1f}% {name}')
    if candidate_total:
        print('==', group, 'stack-scan heuristic; unvalidated candidate',
              'candidate total', candidate_total, 'raw denominator', raw_total)
        for name, n in candidate.most_common(limit):
            print(f'{100*n/max(raw_total,1):5.1f}% of raw samples {name}')
