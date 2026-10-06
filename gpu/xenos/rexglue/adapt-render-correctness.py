#!/usr/bin/env python3
"""Check or apply the isolated shared-memory compute-write access correction.

Use after prepare-archive.py for a fresh source tree, or on an existing copied
tree. Only R-comp/build trees with the pinned preparation manifest are accepted.
The default is a read-only check. No descriptor, shader or timing change is made.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PIN = 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'
RELATIVE = 'src/graphics/vulkan/shared_memory.cpp'
PATCH = ROOT / 'gpu/xenos/rexglue/render-patches/shared-memory-compute-write-access.patch'
RECORD = '.rcomp-render-correctness.json'
OLD = b'''    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_READ_BIT;
      return;'''
NEW = b'''    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      // Resolves write this buffer from compute shaders. Include writes in
      // both access scopes so subsequent reads observe the resolved bytes.
      access_mask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      return;'''


def sha(data):
    return hashlib.sha256(data).hexdigest()


def checked_contents(path):
    data = path.read_bytes()
    # Archives use LF, some copied trees use CRLF. Preserve the source's form.
    if b'\r' in data.replace(b'\r\n', b''):
        raise ValueError('unsupported mixed/bare-CR source newlines')
    newline = b'\r\n' if b'\r\n' in data else b'\n'
    normalized = data.replace(b'\r\n', b'\n')
    if newline == b'\r\n' and b'\n' in data.replace(b'\r\n', b''):
        raise ValueError('mixed source newlines')
    old_count, new_count = normalized.count(OLD), normalized.count(NEW)
    if old_count == 1 and new_count == 0:
        result = normalized.replace(OLD, NEW, 1)
        return data, result.replace(b'\n', newline), False
    if old_count == 0 and new_count == 1:
        return data, data, True
    raise ValueError('unknown or partial compute-write access block; no files changed')


def atomic_write(path, data):
    # Complete all checks before creating the replacement. Existing mode bits
    # remain valid on the Cygwin/Windows host used for preparation.
    descriptor, temporary = tempfile.mkstemp(prefix=path.name+'.rcomp-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, path.stat().st_mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    source = args.source.resolve()
    build = (ROOT/'build').resolve()
    if build not in source.parents:
        parser.error('source must be a prepared directory below R-comp/build')
    try:
        preparation = json.loads((source/'.rcomp-source-manifest.json').read_text())
        if preparation['dependencies']['rexglue-sdk']['commit'] != PIN:
            raise ValueError('unsupported source pin')
        target = (source/RELATIVE).resolve()
        if source not in target.parents:
            raise ValueError('source file resolves outside the prepared tree')
        before, after, fixed = checked_contents(target)
        record_path = source/RECORD
        record = json.loads(record_path.read_text()) if record_path.exists() else {
            'version': 1, 'pin': PIN, 'corrections': {}}
        if record.get('version') != 1 or record.get('pin') != PIN:
            raise ValueError('unsupported correction evidence record')
        correction = record['corrections'].get('shared-memory-compute-write-access')
        patch_sha = sha(PATCH.read_bytes())
        if correction and (correction.get('patch_sha256') != patch_sha or
                           correction.get('after_sha256') != sha(before) or not fixed):
            raise ValueError('correction record disagrees with source or candidate patch')
        evidence = {'file': RELATIVE, 'patch_sha256': patch_sha,
                    'before_sha256': sha(before), 'after_sha256': sha(after),
                    'already_corrected': fixed}
        if not args.apply:
            print('PASS checked compute-write correction; no files changed:', source)
            print(json.dumps(evidence, sort_keys=True))
            return
        if correction:
            print('PASS compute-write correction already recorded:', source)
            return
        if not fixed:
            atomic_write(target, after)
        record['corrections']['shared-memory-compute-write-access'] = evidence
        encoded = (json.dumps(record, indent=2)+'\n').encode('utf-8')
        if record_path.exists():
            atomic_write(record_path, encoded)
        else:
            # The source correction is independently idempotent if interrupted
            # before this new evidence file has been created.
            with record_path.open('xb') as stream:
                stream.write(encoded)
        print('PASS applied isolated compute-write correction:', source)
        print(json.dumps(evidence, sort_keys=True))
    except (KeyError, ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
