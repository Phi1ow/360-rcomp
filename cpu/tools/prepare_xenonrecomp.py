#!/usr/bin/env python3
"""Prepare a fresh patched generator copy, including Windows CRLF checkouts.

Requires the pinned submodules and the public patch utility. Does not modify
the reference checkout or replace an existing output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/cpu-xenonrecomp-host-src')
    args = ap.parse_args()
    dest = args.out.resolve()
    if not dest.is_relative_to((ROOT / 'build').resolve()) or dest.exists():
        ap.error('output must be a fresh directory under build/')
    src = ROOT / 'third_party/XenonRecomp'
    expected = 'ddd128bcca99fe8bfbb99bea583c972351fa6ace'
    actual = subprocess.check_output(['git', '-C', str(src), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != expected:
        ap.error(f'XenonRecomp SHA mismatch: {actual}')
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns('.git'))
    for file in dest.rglob('*'):
        if file.is_file() and file.suffix in ('.cpp', '.h', '.hpp', '.c', '.txt', '.cmake', '.inc'):
            data = file.read_bytes()
            if b'\0' not in data:
                file.write_bytes(data.replace(b'\r\n', b'\n'))
    patches = []
    for patch in sorted((ROOT / 'cpu/patches/xenonrecomp').glob('*.patch')):
        data = patch.read_bytes().replace(b'\r\n', b'\n')
        # cwd instead of -d: a Windows patch (Git for Windows) on a Cygwin PATH cannot open /cygdrive paths.
        subprocess.run(['patch', '--binary', '-p1', '--forward'], input=data, cwd=dest, check=True)
        patches.append({'path': patch.name, 'sha256_lf': hashlib.sha256(data).hexdigest()})
    (dest / 'rcomp-preparation.json').write_text(json.dumps({'upstream': actual, 'patches': patches}, indent=2))


if __name__ == '__main__':
    main()
