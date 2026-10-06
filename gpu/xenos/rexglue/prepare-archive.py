#!/usr/bin/env python3
"""Prepare pinned GPU-only rexglue sources without mutating reference checkouts.

Archive transport for hosts without a populated git/submodule checkout. Pins
come from R-comp's existing deps.lock; patches are the same ones as prepare.sh.
The output must be a new directory under build/. No existing tree is removed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[3]

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def unpack(archive, destination, pin, gpu_only=False):
    with tarfile.open(archive, 'r:gz') as source:
        for member in source.getmembers():
            path = PurePosixPath(member.name)
            if path.is_absolute() or '..' in path.parts or not path.parts:
                raise ValueError('unsafe archive path')
            if not path.parts[0].endswith('-' + pin):
                raise ValueError('archive root does not match the pinned commit')
            relative = PurePosixPath(*path.parts[1:])
            if not relative.parts:
                continue
            name = relative.as_posix()
            if gpu_only:
                allowed = (name.startswith(('include/', 'src/core/', 'src/ui/',
                                             'src/graphics/', 'thirdparty/renderdoc/')) or
                           name in ('LICENSE', 'LICENSE.md'))
                if not allowed or name.startswith('src/graphics/d3d12/'):
                    continue
            if member.isdir():
                (destination / relative).mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as input_stream, target.open('xb') as output_stream:
                    while block := input_stream.read(1024 * 1024):
                        output_stream.write(block)
            elif member.issym() or member.islnk():
                # No linked file is needed by the selected build. Refuse rather
                # than follow archive links into an unrelated directory.
                raise ValueError('linked archive member: ' + name)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--git', default='git', help='Git executable for exact patch application')
    parser.add_argument('--descriptor-reuse', action='store_true',
                        help='Opt in to the separately validated descriptor reuse candidate')
    args = parser.parse_args()
    git_tool = shutil.which(args.git)
    if git_tool is None:
        parser.error('Git was not found; supply --git before preparing the output')
    subprocess.run([git_tool, '--version'], check=True)
    output, cache = args.out.resolve(), args.cache.resolve()
    build = (ROOT / 'build').resolve()
    if build not in output.parents or build not in cache.parents or output.exists():
        parser.error('output must be fresh and both paths must be under R-comp/build')
    entries = {}
    for line in (ROOT / 'deps/deps.lock').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        fields = line.split('|')
        entries[fields[0]] = fields
    names = ('rexglue-sdk', 'rexglue/glslang', 'rexglue/spirv-headers',
             'rexglue/vulkan-headers', 'rexglue/vulkan-memory-allocator',
             'rexglue/xxHash', 'rexglue/fmt', 'rexglue/spdlog', 'rexglue/utfcpp')
    cache.mkdir(parents=True, exist_ok=True)

    def fetch(name):
        fields = entries[name]
        pin = fields[2]
        if len(pin) != 40 or any(c not in '0123456789abcdef' for c in pin):
            raise ValueError('not an immutable commit: ' + name)
        repository = fields[1].removesuffix('.git').removeprefix('https://github.com/')
        if repository.count('/') != 1 or repository.startswith('http'):
            raise ValueError('unsupported archive host')
        archive = cache / (name.replace('/', '-') + '-' + pin + '.tar.gz')
        url = 'https://codeload.github.com/' + repository + '/tar.gz/' + pin
        if not archive.exists():
            part = archive.with_suffix('.part')
            if part.exists():
                raise FileExistsError('unfinished download is retained: ' + str(part))
            request = urllib.request.Request(url, headers={'User-Agent': 'R-comp-pinned-GPU-build'})
            with urllib.request.urlopen(request, timeout=45) as response, part.open('xb') as stream:
                while block := response.read(1024 * 1024):
                    stream.write(block)
            part.rename(archive)
        print('Fetched pinned', name, pin, flush=True)
        return name, archive, {'repository': fields[1], 'commit': pin,
                               'archive_sha256': digest(archive), 'license': fields[3]}

    with ThreadPoolExecutor(max_workers=4) as executor:
        fetched = list(executor.map(fetch, names))
    output.mkdir()
    manifest = {'dependencies': {}, 'patches': {}, 'scope': 'GPU sources; no CPU/JIT/kernel linked'}
    for name, archive, record in fetched:
        target = output if name == 'rexglue-sdk' else output / 'thirdparty' / name.split('/')[1]
        unpack(archive, target, record['commit'], name == 'rexglue-sdk')
        manifest['dependencies'][name] = record
    # This private repository only scopes git apply to the prepared directory;
    # no sources are staged or committed. The R-comp working tree is untouched.
    subprocess.run([git_tool, '-C', str(output), 'init', '-q'], check=True)
    for patch in sorted((ROOT / 'gpu/xenos/rexglue/patches').glob('*.patch')):
        if patch.name == '0012-reuse-texture-descriptors.patch' and not args.descriptor_reuse:
            print('Skipped opt-in performance candidate', patch.name, flush=True)
            continue
        # Windows checkout may carry CRLF patches while codeload carries LF
        # source. Canonicalize only the patch transport, never tracked evidence.
        content = patch.read_bytes().replace(b'\r\n', b'\n')
        command = [git_tool, '-C', str(output), 'apply', '--whitespace=nowarn']
        subprocess.run(command + ['--check', '-'], input=content, check=True)
        subprocess.run(command + ['-'], input=content, check=True)
        manifest['patches'][patch.relative_to(ROOT).as_posix()] = digest(patch)
        print('Applied exact patch', patch.name, flush=True)
    (output / '.rcomp-source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('PASS prepared pinned Xenos GPU sources:', output)

if __name__ == '__main__':
    main()
