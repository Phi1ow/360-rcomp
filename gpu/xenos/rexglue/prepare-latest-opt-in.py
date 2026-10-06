#!/usr/bin/env python3
"""Prepare the reviewed current GPU source chain in a new R-comp build tree.

Descriptor reuse is opt-in. Upload shadows, CPU scratch, atomic counters and
GPU checkpoints are never applied by this script. R-comp glue already belongs
in the repository and is verified rather than patched twice. Compilation and
PS5 execution remain separate validation gates.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

PACKAGE = Path(__file__).resolve().parent

def digest(path, normalize=False):
    data = path.read_bytes()
    if normalize:
        data = data.replace(b'\r\n', b'\n')
    return hashlib.sha256(data).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo-root', type=Path, default=PACKAGE.parents[2],
                        help='R-comp checkout root; explicit only for candidate-copy validation')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--git', default='git')
    parser.add_argument('--descriptor-reuse', action='store_true',
                        help='Explicitly enable0012, matching the measured phase15+ chain')
    parser.add_argument('--offline', action='store_true',
                        help='Require all verified public archives in cache; never download')
    args = parser.parse_args()
    if sys.platform == 'win32':
        parser.error('Use POSIX/Cygwin Python and FULL Cygwin Git, not native Windows git apply')
    root = args.repo_root.resolve()
    build = (root/'build').resolve()
    output, cache = args.out.resolve(), args.cache.resolve()
    if build not in output.parents or build not in cache.parents or output.exists():
        parser.error('output must be fresh and both paths must resolve below R-comp/build')
    profile = json.loads((PACKAGE/'latest-source-profile.json').read_text())
    if profile.get('version') != 1:
        parser.error('unsupported source profile version')
    for rel, expected in profile['required_repo_inputs'].items():
        if digest(root/rel, True) != expected:
            parser.error('repository GPU input differs from reviewed profile: '+rel)
    entries = {}
    for line in (root/'deps/deps.lock').read_text().splitlines():
        if line and not line.startswith('#'):
            fields=line.split('|');entries[fields[0]]=fields
    archive_hashes = {}
    for name, dep in profile['dependencies'].items():
        fields=entries.get(name)
        if not fields or fields[1] != dep['repository'] or fields[2] != dep['commit']:
            parser.error('dependency lock differs from reviewed pin: '+name)
        archive=cache/(name.replace('/','-')+'-'+dep['commit']+'.tar.gz')
        archive_hashes[archive.name]=dep['archive_sha256']
        if args.offline and not archive.is_file():
            parser.error('offline archive missing: '+str(archive))
        if archive.exists() and digest(archive) != dep['archive_sha256']:
            parser.error('cached archive hash mismatch: '+str(archive))
    # Patch text identities tolerate Git checkout line endings; archives stay byte-exact.
    for patch in profile['ordered_patches']:
        if digest(PACKAGE/patch['path'], True) != patch['sha256']:
            parser.error('patch differs from reviewed profile: '+patch['path'])
    for patch in profile['ordered_patches'][:profile['base_patch_count']]:
        if digest(root/'gpu/xenos/rexglue'/patch['path'], True) != patch['sha256']:
            parser.error('base preparation patch differs: '+patch['path'])
    helper = PACKAGE/'prepare-archive.py'
    spec = importlib.util.spec_from_file_location('rcomp_gpu_archive_transport', helper)
    module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    module.ROOT = root
    original_digest = module.digest
    def verified_archive_digest(path):
        value = original_digest(path)
        expected=archive_hashes.get(path.name)
        if expected is not None and value != expected:
            raise ValueError('downloaded archive hash mismatch: '+str(path))
        return value
    module.digest = verified_archive_digest
    original_argv=sys.argv
    try:
        sys.argv=[str(helper),'--out',str(output),'--cache',str(cache),'--git',args.git]
        # Base preparation excludes0012; the ordered profile owns its position.
        module.main()
    finally:
        sys.argv=original_argv
    applied=[]
    for patch in profile['ordered_patches'][profile['base_patch_count']:]:
        if patch['descriptor_reuse_only'] and not args.descriptor_reuse:
            print('Skipped explicit opt-in',patch['path'],flush=True)
            continue
        content=(PACKAGE/patch['path']).read_bytes().replace(b'\r\n',b'\n')
        command=[args.git,'-C',str(output),'apply','--ignore-space-change','--whitespace=nowarn']
        subprocess.run(command+['--check','-'],input=content,check=True)
        subprocess.run(command+['-'],input=content,check=True)
        applied.append(patch)
        print('Applied reviewed patch',patch['path'],flush=True)
    expected=profile['source_hashes_descriptor_on' if args.descriptor_reuse else 'source_hashes_descriptor_off']
    actual={p.relative_to(output).as_posix():digest(p,True)
            for folder in ('include','src') for p in (output/folder).rglob('*')
            if p.is_file() and p.suffix in ('.h','.hpp','.cpp','.c')}
    if actual != expected:
        mismatched=sorted(set(actual)^set(expected) | {k for k in actual.keys()&expected.keys() if actual[k]!=expected[k]})
        (output/'.rcomp-latest-source-FAIL.json').write_text(json.dumps(mismatched,indent=2)+'\n')
        raise ValueError('prepared sources differ from reviewed profile; see retained FAIL evidence')
    record={'status':'PASS','profile_sha256':digest(PACKAGE/'latest-source-profile.json'),
            'descriptor_reuse':args.descriptor_reuse,'ordered_additional_patches':applied,
            'normalized_source_hashes':actual,'recommended_defaults':profile['defaults'],
            'validation_scope':'Exact public source preparation, not compilation or PS5 proof'}
    (output/'.rcomp-latest-source-manifest.json').write_text(json.dumps(record,indent=2)+'\n')
    print('PASS prepared',len(actual),'exact normalized GPU source files:',output,flush=True)

if __name__ == '__main__':
    main()
