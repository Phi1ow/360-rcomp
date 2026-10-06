#!/usr/bin/env python3
"""PowerPC-instruction profile of a PC-sampler run (RCOMP-PC lines of a title log) of a build with line tables.

The title's recompiled code is C++ in which every PowerPC instruction is a '// mnemonic operands' comment followed by the
statements that implement it (XenonRecomp output, ppc_recomp.N.cpp). A build compiled with -gline-tables-only (for example
RCOMP_TLS_FLAGS=-gline-tables-only for build/prime-tbogt-20261002/tbogt_ps5.sh) lets llvm-symbolizer turn each sampled host
instruction pointer into a C++ line of one of those files, and the comment above that line names the PowerPC instruction.
With inlining the innermost frame names the helper the host instruction came from (generated_guest_load, note_guest_write,
disableFlushMode, the call-lookup of an indirect branch, ...): which translation overhead costs time, per PowerPC instruction.

usage: pc_ppc_profile.py --log pc-window.log --elf title.pie.elf --map title.map [--group main] [--ppc-dir DIR] [--top 40]
       [--symbolizer PATH]
Without line tables the report still gives the function profile and says that the line mapping is unavailable.
"""
import argparse
import bisect
import collections
import json
import os
import re
import subprocess
import sys

MNEMONIC = re.compile(r'^\t// ([a-z][a-z0-9_.]*)(?: (.*))?$')
PPC_FILE = re.compile(r'ppc_recomp\.\d+\.cpp$')

CATEGORIES = [
    ('call (bl/bctrl)', {'bl', 'bctrl', 'bla', 'blrl'}),
    ('return/indirect branch', {'blr', 'bctr', 'bdnz', 'bdz'}),
    ('branch', {'b', 'beq', 'bne', 'blt', 'bgt', 'ble', 'bge', 'bso', 'bns', 'bnl', 'bng', 'bdnzf', 'bdnzt', 'bf', 'bt', 'bdzf', 'bdzt', 'beqlr', 'bnelr', 'bltlr', 'bgtlr', 'blelr', 'bgelr'}),
    ('compare', {'cmpw', 'cmplw', 'cmpwi', 'cmplwi', 'cmpd', 'cmpld', 'cmpdi', 'cmpldi', 'fcmpu', 'fcmpo', 'vcmpgtfp', 'vcmpeqfp', 'vcmpgefp', 'vcmpequw'}),
    ('load', {'lwz', 'lbz', 'lhz', 'lha', 'ld', 'lwzx', 'lbzx', 'lhzx', 'lwa', 'ldx', 'lwzu', 'lbzu', 'lhzu', 'lwbrx', 'lhbrx', 'lwarx', 'ldarx'}),
    ('store', {'stw', 'stb', 'sth', 'std', 'stwx', 'stbx', 'sthx', 'stwu', 'stbu', 'sthu', 'stdu', 'stwbrx', 'sthbrx', 'stwcx.', 'stdcx.', 'stdx'}),
    ('fp load/store', {'lfs', 'lfd', 'lfsx', 'lfdx', 'stfs', 'stfd', 'stfsx', 'stfdx', 'lfsu', 'stfsu', 'lfdu', 'stfdu'}),
    ('vector load/store', {'lvx', 'lvx128', 'stvx', 'stvx128', 'lvlx', 'lvrx', 'lvlx128', 'lvrx128', 'stvlx', 'stvrx', 'stvlx128', 'stvrx128', 'lvewx', 'stvewx', 'lvsl', 'lvsr'}),
    ('prologue/epilogue', {'mflr', 'mtlr', 'stwu', 'mtctr', 'mfctr'}),
]
MNEMONIC_CATEGORY = {}
for name, members in CATEGORIES:
    for m in members:
        MNEMONIC_CATEGORY.setdefault(m, name)


def category(mnemonic):
    if not mnemonic:
        return 'unattributed'
    if mnemonic in MNEMONIC_CATEGORY:
        return MNEMONIC_CATEGORY[mnemonic]
    if mnemonic.startswith('v'):
        return 'vector alu'
    if mnemonic.startswith('f'):
        return 'fp alu'
    return 'integer alu/move'


def parse_log(path, group):
    """{rip: count} of the interrupted instruction pointers of one group, and the relocated text range."""
    counts = collections.Counter()
    rx = None
    with open(path, errors='replace') as f:
        for line in f:
            if 'RCOMP-PC-FORMAT ' in line:
                values = dict(re.findall(r'\b(rx_begin|rx_end|rx_valid)=([0-9a-f]+)', line))
                if values.get('rx_valid') == '1':
                    rx = (int(values['rx_begin'], 16), int(values['rx_end'], 16))
                continue
            m = re.match(r'RCOMP-PC (\S+) total=\d+ recorded=\d+ dropped=\d+(.*)$', line.strip())
            if not m or m.group(1) != group:
                continue
            for tok in m.group(2).split():
                a, _, c = tok.partition(':')
                if not c:
                    continue
                rip = int(a, 16)
                if rip >> 63:
                    continue  # stack-scan heuristic candidates are never added to the raw counts
                counts[rip] += int(c)
    return counts, rx


def load_bias_from_map(map_path, rx):
    text = None
    for line in open(map_path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+)\s+(\.text)\s*$', line)
        if m:
            text = (int(m[1], 16), int(m[1], 16) + int(m[3], 16))
            break
    if text is None or rx is None or rx[1] - rx[0] != text[1] - text[0]:
        return None
    return rx[0] - text[0]


def host_path(path):
    """A Cygwin path as recorded in the debug info -> a path this Python can open."""
    m = re.match(r'^/cygdrive/([a-zA-Z])/(.*)$', path)
    if m:
        return f'{m.group(1).upper()}:/{m.group(2)}'
    return path


class SourceLines:
    def __init__(self, ppc_dir=None):
        self.cache = {}
        self.ppc_dir = ppc_dir

    def lines(self, path):
        if path not in self.cache:
            candidates = [host_path(path)]
            if self.ppc_dir:
                candidates.append(os.path.join(self.ppc_dir, os.path.basename(path)))
            data = None
            for c in candidates:
                try:
                    with open(c, errors='replace') as f:
                        data = f.read().split('\n')
                    break
                except OSError:
                    continue
            self.cache[path] = data
        return self.cache[path]

    def instruction_at(self, path, line):
        """(mnemonic, full comment text) of the PowerPC instruction the C++ line implements: the nearest comment above."""
        lines = self.lines(path)
        if not lines or line <= 0 or line > len(lines):
            return None, None
        for i in range(line - 1, max(-1, line - 40), -1):
            m = MNEMONIC.match(lines[i])
            if m:
                return m.group(1), (m.group(1) + (' ' + m.group(2) if m.group(2) else ''))
            if lines[i].startswith(('PPC_FUNC_IMPL', 'loc_')):
                break
        return None, None


def symbolize(symbolizer, elf, addresses):
    """{address: [frame, ...]} innermost frame first; a frame is {'fn', 'file', 'line'}."""
    out = {}
    if not addresses:
        return out
    proc = subprocess.run([symbolizer, '--output-style=JSON', '--inlines', '--functions=linkage', '--obj=' + elf],
                          input='\n'.join('0x%x' % a for a in addresses) + '\n', capture_output=True, text=True, errors='replace')
    if proc.returncode != 0:
        raise SystemExit('llvm-symbolizer failed: ' + proc.stderr[:300])
    rows = [json.loads(l) for l in proc.stdout.splitlines() if l.strip().startswith('{')]
    for a, row in zip(addresses, rows):
        frames = [{'fn': s.get('FunctionName', '??'), 'file': s.get('FileName', '??'), 'line': int(s.get('Line', 0) or 0)}
                  for s in row.get('Symbol', [])]
        out[a] = frames
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--map', required=True)
    ap.add_argument('--group', default='main', choices=['main', 'others', 'gpu'])
    ap.add_argument('--ppc-dir', default=None)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--symbolizer', default='llvm-symbolizer')
    a = ap.parse_args()
    counts, rx = parse_log(a.log, a.group)
    bias = load_bias_from_map(a.map, rx)
    total = sum(counts.values())
    if not total or bias is None:
        raise SystemExit(f'no usable samples for group {a.group} (total {total}, load bias {bias})')
    addrs = sorted({rip - bias for rip in counts if rx and rx[0] <= rip < rx[1]})
    frames = symbolize(a.symbolizer, a.elf, addrs)
    src = SourceLines(a.ppc_dir)
    by_function = collections.Counter()
    by_mnemonic = collections.Counter()
    by_category = collections.Counter()
    by_helper = collections.Counter()
    by_pair = collections.Counter()
    by_instruction = collections.defaultdict(collections.Counter)  # function -> 'comment' -> count
    outside = 0
    mapped_lines = 0
    for rip, n in counts.items():
        if not (rx and rx[0] <= rip < rx[1]):
            outside += n
            continue
        fr = frames.get(rip - bias) or []
        if not fr:
            outside += n
            continue
        outer, inner = fr[-1], fr[0]
        fn = outer['fn']
        by_function[fn] += n
        if PPC_FILE.search(outer['file']) and outer['line'] > 0:
            mapped_lines += n
            mn, comment = src.instruction_at(outer['file'], outer['line'])
            by_mnemonic[mn or '?'] += n
            by_category[category(mn)] += n
            helper = inner['fn'] if inner is not outer and inner['fn'] != outer['fn'] else 'body'
            by_helper[helper] += n
            by_pair[(mn or '?', helper)] += n
            if comment:
                by_instruction[fn][comment] += n
        else:
            by_category['runtime/other'] += n

    def pct(x):
        return f'{100.0 * x / total:6.2f} %'

    print(f'group {a.group}: {total} samples ({outside} outside the verified title text); {mapped_lines} samples mapped to a PowerPC instruction ({100.0 * mapped_lines / total:.1f} %)')
    print('\n== by function')
    for fn, n in by_function.most_common(a.top):
        print(f'{pct(n)}  {fn[:110]}')
    if mapped_lines:
        print('\n== by PowerPC instruction category')
        for k, n in by_category.most_common():
            print(f'{pct(n)}  {k}')
        print('\n== by PowerPC mnemonic')
        for k, n in by_mnemonic.most_common(a.top):
            print(f'{pct(n)}  {k}')
        print('\n== by translation helper (innermost inlined frame; "body" = the statement itself)')
        for k, n in by_helper.most_common(a.top):
            print(f'{pct(n)}  {k[:110]}')
        print('\n== by (mnemonic, helper)')
        for (mn, h), n in by_pair.most_common(a.top):
            print(f'{pct(n)}  {mn:10} {h[:90]}')
        print('\n== hottest PowerPC instructions of the hottest functions')
        for fn, _ in by_function.most_common(8):
            if fn not in by_instruction:
                continue
            print(f'-- {fn[:90]}  ({pct(by_function[fn]).strip()})')
            for comment, n in by_instruction[fn].most_common(6):
                print(f'     {pct(n)}  {comment[:80]}')
    else:
        print('\n(no line tables in the build: compile with -gline-tables-only, e.g. RCOMP_TLS_FLAGS=-gline-tables-only, for the instruction profile)')


if __name__ == '__main__':
    sys.exit(main())
