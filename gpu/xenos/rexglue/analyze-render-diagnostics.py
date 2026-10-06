#!/usr/bin/env python3
"""Summarize bounded RCOMP-RENDER states; this is observation, not a pixel oracle."""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import re


def parse(line):
    match = re.search(r'RCOMP-RENDER-([A-Z-]+) (.*)', line)
    if not match:
        return None
    return match.group(1), dict(re.findall(r'(\w+)=([^\s]+)', match.group(2)))


def target_key(text):
    # Layout from pinned RenderTargetCache::RenderTargetKey.
    word = int(text, 16)
    return {'key': text, 'base_tiles': word & 2047,
            'pitch_tiles_32bpp': (word >> 11) & 255,
            'msaa_log2': (word >> 19) & 3, 'is_depth': bool((word >> 21) & 1),
            'format': (word >> 22) & 15}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    data = args.log.read_bytes()
    lines = data.decode('utf-8', errors='replace').splitlines()
    records = [entry for line in lines if (entry := parse(line))]
    counts = collections.Counter(kind for kind, _ in records)
    draws, transfers = [], []
    for kind, fields in records:
        identity = tuple(fields.get(key) for key in ('frame', 'sub', 'vs', 'ps'))
        if kind == 'DEPTH':
            # DRAW/DEPTH are adjacent in the producer. The same frame, shader
            # and submission may have multiple distinct viewport/depth states.
            if draws and draws[-1]['identity'] == identity:
                draws[-1]['depth'] = fields
        elif kind == 'DRAW':
            draws.append({'fields': fields, 'identity': identity, 'depth': {}})
        elif kind == 'TRANSFER':
            transfers.append({'fields': fields, 'source': target_key(fields['source']),
                              'dest': target_key(fields['dest'])})
    classes = collections.Counter()
    observations = []
    for draw in draws:
        fields = draw['fields']
        depth = draw['depth']
        color = int(fields['normcolor'], 16)
        ztest, zwrite, zfunc = (int(depth.get(key, '0')) for key in ('ztest', 'zwrite', 'zfunc'))
        label = ('depth_write_without_color' if not color and zwrite else
                 'color_equal_depth' if color and ztest and zfunc == 2 else
                 'color_other_depth' if color else 'no_color_no_depth_write')
        classes[label] += 1
        nonfinite = [key for key in ('z', 'ndcs', 'ndco')
                     if any(not math.isfinite(float(value)) for value in fields.get(key, '').split(',') if value)]
        observations.append({'class': label, 'draw': fields, 'depth': depth,
                             'nonfinite_host_values': nonfinite})
    truncated = [line for line in lines if 'RCOMP-RENDER-DIAG limit ' in line]
    result = {'status': 'PASS' if records else 'NOT TESTED',
              'scope': 'parsed observed states; pixels/cause/performance are not inferred',
              'source': str(args.log.resolve()), 'source_sha256': hashlib.sha256(data).hexdigest(),
              'record_counts': dict(counts), 'draw_state_classes': dict(classes),
              'truncation': truncated, 'draw_states': observations, 'transfers': transfers}
    encoded = json.dumps(result, indent=2)+'\n'
    if args.out:
        args.out.write_text(encoded, encoding='utf-8')
        print(result['status'], 'observed states:', dict(counts), 'classes:', dict(classes),
              'truncated categories:', len(truncated), '->', args.out)
    else:
        print(encoded, end='')


if __name__ == '__main__':
    main()
