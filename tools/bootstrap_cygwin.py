#!/usr/bin/env python3
"""Extract the locked Windows host toolchain under build/; no installer/hooks.

Requires Python 3.12 and zstandard==0.25.0:
    python -m pip install --target build/prime-host-tools/pylibs zstandard==0.25.0
No registry, services, shared SDK, PATH setting or console is modified.
The package archives stay local; their SHA-512 and size are checked before use.

The packages' postinstall hooks are not run. That leaves two gaps, which this script closes after the verified
extraction, inside the extracted root only:
  * llvm-config and rev. The ps5-payload SDK's wrappers (prospero-clang, prospero-lld, ...) ask `llvm-config --bindir`
    (where clang lives) and `llvm-config --version` (clang >= 20 appends the crt objects itself), and cut a tool name
    with `rev`. The lock has the LLVM tools but not the llvm-devel and util-linux packages that provide them: two small
    shell scripts stand in (bin/llvm-config, bin/rev). Without them the SDK compiler stops with "rev: command not
    found" and "/llvm-: No such file or directory"; with a llvm-config that does not report the version, linking
    fails on duplicate crt symbols.
  * CA certificates. The packages ship empty bundles that `update-ca-trust` fills. Without it Cygwin's git and curl
    cannot verify an HTTPS server. `update-ca-trust` runs once (about a minute); --skip-ca-certificates leaves it out.
"""
import argparse
import concurrent.futures
import hashlib
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request

REPO = pathlib.Path(__file__).resolve().parents[1]


def llvm_version(lock):
    for package in lock['packages']:
        if package['name'] == 'llvm':
            return package['version'].split('-')[0]
    raise ValueError('deps/cygwin-host.lock.json has no llvm package')


def sdk_shims(lock):
    """bin/ scripts that stand in for packages the lock does not carry: {path under the Cygwin root: script text}."""
    llvm_config = f"""#!/usr/bin/bash
# Written by tools/bootstrap_cygwin.py. The locked Cygwin has the LLVM tools but not llvm-devel, which provides
# llvm-config; the ps5-payload SDK wrappers only ask for the options below.
case "$1" in
    --version) echo {llvm_version(lock)} ;;
    --bindir) echo /usr/bin ;;
    --prefix) echo /usr ;;
    --libdir) echo /usr/lib ;;
    --includedir) echo /usr/include ;;
    *) echo "llvm-config (bootstrap_cygwin.py stand-in): unsupported option: $*" >&2; exit 1 ;;
esac
"""
    rev = """#!/usr/bin/bash
# Written by tools/bootstrap_cygwin.py. util-linux's rev (reverse every line) is not in the locked package set; the
# ps5-payload SDK wrappers use it to cut a tool name at its last '-'.
exec /usr/bin/gawk '{ r = ""; for (i = length($0); i > 0; i--) r = r substr($0, i, 1); print r }' "$@"
"""
    return {'bin/llvm-config': llvm_config, 'bin/rev': rev}


def write_shims(root, lock):
    """Write the stand-in scripts under the Cygwin root (LF line endings: Cygwin's bash cannot run CRLF scripts)."""
    shims = sdk_shims(lock)
    for rel, text in shims.items():
        path = root / rel
        path.write_bytes(text.encode('utf-8'))
        path.chmod(0o755)
    return sorted(shims)


def install_ca_bundle(root):
    """Fill the CA bundles with update-ca-trust and copy the result where git and curl look. False when it failed."""
    try:
        result = subprocess.run([str(root / 'bin/bash.exe'), '--noprofile', '--norc', '-c',
                                 'export PATH=/usr/bin:/bin; update-ca-trust'], capture_output=True, text=True, timeout=900)
    except (OSError, subprocess.SubprocessError):
        return False
    bundle = root / 'etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem'
    if result.returncode != 0 or not bundle.is_file() or bundle.stat().st_size == 0:
        return False
    for rel in ('etc/pki/tls/certs/ca-bundle.crt', 'etc/pki/tls/cert.pem'):
        target = root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(bundle, target)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=pathlib.Path, default=REPO / 'build/prime-host-tools')
    ap.add_argument('--skip-ca-certificates', action='store_true',
                    help='do not run update-ca-trust (Cygwin git and curl then cannot verify HTTPS servers)')
    args = ap.parse_args()
    base = args.out.resolve()
    if not base.is_relative_to((REPO / 'build').resolve()):
        ap.error('toolchain output must stay under this repository build/')
    sys.path.insert(0, str(base / 'pylibs'))
    try:
        import zstandard
    except ImportError:
        ap.error(f'zstandard 0.25.0 is missing: python -m pip install --target "{base / "pylibs"}" zstandard==0.25.0')
    if zstandard.__version__ != '0.25.0':
        ap.error('zstandard 0.25.0 is required')
    lock = json.loads((REPO / 'deps/cygwin-host.lock.json').read_text())
    cache = base / 'cache'
    cache.mkdir(parents=True, exist_ok=True)

    def fetch(package):
        path = cache / pathlib.PurePosixPath(package['path']).name
        if not path.exists():
            with urllib.request.urlopen(lock['mirror'] + package['path'], timeout=120) as source, path.open('wb') as out:
                shutil.copyfileobj(source, out)
        data = path.read_bytes()
        if len(data) != package['size'] or hashlib.sha512(data).hexdigest() != package['sha512']:
            raise ValueError(f'archive size/hash mismatch: {path}')
        return path

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        paths = list(pool.map(fetch, lock['packages']))
    root = base / 'cygwin'
    if root.exists():
        ap.error('destination exists; choose a fresh --out (existing tools are never replaced)')
    root.mkdir()

    def mapped(name):
        name = name.removeprefix('./').lstrip('/')
        # Cygwin automatically mounts root/bin and root/lib under /usr too.
        if name.startswith(('usr/bin/', 'usr/lib/')):
            name = name[4:]
        path = (root / name).resolve()
        if not path.is_relative_to(root):
            raise ValueError('archive path escapes toolchain: ' + name)
        return path

    links = []
    for path in paths:
        stream = None
        if path.suffix == '.zst':
            with path.open('rb') as raw, zstandard.ZstdDecompressor().stream_reader(raw) as source:
                stream = io.BytesIO(source.read())
        with (tarfile.open(fileobj=stream) if stream else tarfile.open(path)) as archive:
            for member in archive:
                # Documentation includes names such as ':', illegal on Windows.
                if member.name.startswith(('usr/share/man/', 'usr/share/doc/')):
                    continue
                dest = mapped(member.name)
                if member.isdir():
                    dest.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    with archive.extractfile(member) as source, dest.open('wb') as out:
                        shutil.copyfileobj(source, out)
                elif member.issym() or member.islnk():
                    links.append((dest, member))
    # Materialize package aliases without requiring Windows symlink privileges.
    for _ in range(8):
        remaining = []
        for dest, member in links:
            target = (mapped(member.linkname) if member.islnk() or member.linkname.startswith('/')
                      else (dest.parent / member.linkname).resolve())
            if not target.is_relative_to(root):
                raise ValueError('archive link escapes toolchain')
            if target.is_file():
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(target, dest)
            elif target.is_dir() and target != dest:
                shutil.copytree(target, dest, dirs_exist_ok=True)
            else:
                remaining.append((dest, member))
        if len(remaining) == len(links):
            break
        links = remaining
    # The Python package's alternatives hook is deliberately not run.
    shutil.copyfile(root / 'bin/python3.12.exe', root / 'bin/python3.exe')
    (root / 'tmp').mkdir(exist_ok=True)
    (root / 'etc').mkdir(exist_ok=True)
    (root / 'etc/fstab').write_text('none /cygdrive cygdrive binary,posix=0,user 0 0\n')
    stand_ins = write_shims(root, lock)
    ca = False if args.skip_ca_certificates else install_ca_bundle(root)
    if not ca and not args.skip_ca_certificates:
        print('WARNING: update-ca-trust failed: Cygwin git and curl cannot verify HTTPS servers '
              '(use a Windows git for the network steps: RCOMP_GIT)', file=sys.stderr)
    summary = {'root': str(root), 'verified_packages': len(paths), 'stand_in_scripts': stand_ins,
               'ca_certificates': ca, 'unused_package_aliases': [str(p.relative_to(root)) for p, _ in links]}
    (root / 'etc/rcomp-bootstrap-complete').write_text(json.dumps(summary) + '\n')  # written last: an interrupted run has none
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
