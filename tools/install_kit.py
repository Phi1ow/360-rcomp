#!/usr/bin/env python3
"""Download, verify and extract the prebuilt PS5 toolchain kit listed in deps/kit.lock.json.

The kit holds what R-comp does not build itself: the ps5-payload SDK, Mesa RADV for the PS5, compiler-rt builtins,
the FFmpeg XMA codec, ps5-native-tool and libc.prx, the prepared Xenos source, and the PS5_Vulkan link inputs.
Everything lands under build/ at the root of the checkout. Python standard library only.

    python tools/install_kit.py                download the release assets, check them, extract them
    python tools/install_kit.py --from DIR     use archives already downloaded into DIR
    python tools/install_kit.py --check        only report which key files are present

Why a script and not `tar -xzf`: the archives contain symbolic links (the SDK's header aliases such as
target/include/errno.h -> sys/errno.h). A tar run from Git Bash or PowerShell cannot create them without special
privileges, and the SDK is then left without errno.h, fcntl.h, float.h and others (seen on 4 October 2026). This script
writes a copy of the target for every link instead, the same way tools/bootstrap_cygwin.py does.

Every archive is checked against its size and SHA-256 from the lock before it is opened; members that would land
outside build/ are refused. The release of a private repository cannot be fetched anonymously: the script then
falls back to the GitHub CLI (`gh auth login` once), or accepts the archives with --from.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request

REPO = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_LOCK = REPO / 'deps' / 'kit.lock.json'
ROOT_PREFIX = 'build'  # every member must live under this folder of the checkout


class KitError(Exception):
    pass


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def archive_matches(path, asset):
    return path.is_file() and path.stat().st_size == asset['size'] and sha256_of(path) == asset['sha256']


def checked_member_path(name):
    """The POSIX path of an archive member relative to the checkout, or KitError when it would escape build/."""
    if '\\' in name or ':' in name.split('/')[0]:
        raise KitError(f'archive member with a Windows-style path: {name!r}')
    parts = [p for p in name.split('/') if p not in ('', '.')]
    if not parts or parts[0] != ROOT_PREFIX or '..' in parts:
        raise KitError(f'archive member outside {ROOT_PREFIX}/: {name!r}')
    return '/'.join(parts)


def link_target(rel, member):
    """Where a link member points, relative to the checkout (symbolic links are relative to their folder)."""
    if member.linkname.startswith('/') or '\\' in member.linkname or ':' in member.linkname.split('/')[0]:
        raise KitError(f'link with an absolute or Windows-style target: {member.name} -> {member.linkname}')
    base = [] if member.islnk() else rel.split('/')[:-1]
    out = base
    for part in member.linkname.split('/'):
        if part in ('', '.'):
            continue
        if part == '..':
            if not out:
                raise KitError(f'link escapes the checkout: {member.name} -> {member.linkname}')
            out = out[:-1]
        else:
            out = out + [part]
    target = '/'.join(out)
    if not target.startswith(ROOT_PREFIX + '/'):
        raise KitError(f'link leaves {ROOT_PREFIX}/: {member.name} -> {member.linkname}')
    return target


def remove_existing(path):
    """Clear a file or a link before writing it (a link left by another tar must not be written through)."""
    if os.path.lexists(path) and (os.path.islink(path) or not os.path.isdir(path)):
        try:
            os.chmod(path, 0o666)
        except OSError:
            pass
        os.unlink(path)


def extract(archive, root, log):
    """Extract one archive under root; links become copies of their targets. Returns (files, links)."""
    files = 0
    links = []
    with tarfile.open(archive, 'r:gz') as tar:
        for member in tar:
            rel = checked_member_path(member.name)
            dest = root / rel
            if member.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            elif member.isreg():
                dest.parent.mkdir(parents=True, exist_ok=True)
                remove_existing(dest)
                with tar.extractfile(member) as src, open(dest, 'wb') as out:
                    shutil.copyfileobj(src, out)
                if os.name != 'nt':
                    os.chmod(dest, member.mode & 0o777)
                os.utime(dest, (member.mtime, member.mtime))
                files += 1
            elif member.issym() or member.islnk():
                links.append((rel, link_target(rel, member)))
            else:
                raise KitError(f'unsupported archive member type: {member.name}')
    # A link may point to another link: copy what already exists, repeat until nothing changes.
    pending = links
    for _ in range(8):
        left = []
        for rel, target in pending:
            src, dest = root / target, root / rel
            if os.path.lexists(dest) and src.exists() and os.path.samefile(src, dest):
                continue  # already a link to its target (an earlier extraction with a tar that makes links)
            if src.is_file():
                dest.parent.mkdir(parents=True, exist_ok=True)
                remove_existing(dest)
                shutil.copyfile(src, dest)
            elif src.is_dir():
                if dest.exists() and not dest.is_dir():
                    remove_existing(dest)
                shutil.copytree(src, dest, dirs_exist_ok=True)
            else:
                left.append((rel, target))
        if len(left) == len(pending):
            break
        pending = left
    if pending:
        raise KitError('links whose target is not in the archives: ' + ', '.join(f'{r} -> {t}' for r, t in pending[:5]))
    log(f'  {files} files, {len(links)} links written as copies')
    return files, len(links)


def download(asset, lock, dest, log):
    """Fetch one release asset into dest: direct HTTPS first (public repositories), then the GitHub CLI."""
    url = f'https://github.com/{lock["repository"]}/releases/download/{lock["tag"]}/{asset["name"]}'
    part = dest.with_name(dest.name + '.part')
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        log(f'  downloading {url}')
        with urllib.request.urlopen(url, timeout=60) as src, open(part, 'wb') as out:
            done, last = 0, -1
            while True:
                block = src.read(1 << 20)
                if not block:
                    break
                out.write(block)
                done += len(block)
                pct = done * 100 // max(asset['size'], 1)
                if asset['size'] > (8 << 20) and pct // 10 != last // 10:
                    log(f'    {pct}% of {asset["size"] / (1 << 20):.1f} MiB')
                    last = pct
        os.replace(part, dest)
        return
    except (urllib.error.URLError, OSError) as e:
        part.unlink(missing_ok=True)
        log(f'  direct download failed ({e}); trying the GitHub CLI')
    gh = shutil.which('gh')
    if not gh:
        raise KitError(f'cannot download {asset["name"]}: the release is private or unreachable and `gh` is not installed. '
                       f'Download it from {url} in a browser into {dest.parent} and run again (or use --from).')
    proc = subprocess.run([gh, 'release', 'download', lock['tag'], '--repo', lock['repository'], '--pattern', asset['name'],
                           '--dir', str(dest.parent), '--clobber'], capture_output=True, text=True)
    if proc.returncode != 0 or not dest.is_file():
        raise KitError(f'gh release download failed for {asset["name"]}: {(proc.stderr or proc.stdout).strip()[:300]}')


def missing_files(lock, root):
    return [p for asset in lock['assets'] for p in asset.get('provides', []) if not (root / p).exists()]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lock', type=pathlib.Path, default=DEFAULT_LOCK)
    ap.add_argument('--root', type=pathlib.Path, default=REPO, help='checkout root (default: this repository)')
    ap.add_argument('--cache', type=pathlib.Path, help='where downloaded archives are kept (default: <root>/build/kit-cache)')
    ap.add_argument('--from', dest='source', type=pathlib.Path, help='folder that already holds the archives')
    ap.add_argument('--check', action='store_true', help='only report which key files are present')
    args = ap.parse_args(argv)

    def log(text):
        print(text, flush=True)

    root = args.root.resolve()
    lock = json.loads(args.lock.read_text(encoding='utf-8'))
    if args.check:
        gone = missing_files(lock, root)
        for p in gone:
            log(f'MISSING {p}')
        log('PASS the kit is in place' if not gone else f'FAIL {len(gone)} key files missing: run tools/install_kit.py')
        return 1 if gone else 0

    cache = (args.cache or root / 'build' / 'kit-cache').resolve()
    try:
        for asset in lock['assets']:
            name = asset['name']
            log(f'{name} ({asset["size"] / (1 << 20):.1f} MiB)')
            path = (args.source / name) if args.source else (cache / name)
            if not archive_matches(path, asset):
                if args.source:
                    raise KitError(f'{path} is missing or does not match the lock (size {asset["size"]}, sha256 {asset["sha256"]})')
                download(asset, lock, path, log)
                if not archive_matches(path, asset):
                    path.unlink(missing_ok=True)
                    raise KitError(f'{name}: the download does not match the lock (size {asset["size"]}, sha256 {asset["sha256"]})')
            log('  size and SHA-256 match the lock')
            for stale in asset.get('replace', []):
                target = (root / checked_member_path(stale)).resolve()
                if target.is_dir() and (root / ROOT_PREFIX).resolve() in target.parents:
                    log(f'  replacing {stale} (an older copy would mix versions)')
                    shutil.rmtree(target)
            extract(path, root, log)
    except (KitError, tarfile.TarError, OSError) as e:
        log(f'FAIL {e}')
        return 2
    gone = missing_files(lock, root)
    for p in gone:
        log(f'MISSING {p}')
    log('PASS the kit is extracted and its key files are present' if not gone else f'FAIL {len(gone)} key files missing')
    return 1 if gone else 0


if __name__ == '__main__':
    sys.exit(main())
