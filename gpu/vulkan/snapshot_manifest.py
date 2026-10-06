#!/usr/bin/env python3
# R-comp - Agent 4 (PS5_Vulkan integration).
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy PS5_Vulkan inputs into a stable snapshot and write a manifest.

The driver may be rebuilt concurrently, and /home/user/deps checkouts are
shared and read-only, so nothing links against them in place: a consumer
links against a snapshot whose every byte is recorded.

Profiles
  tree    every git-tracked file of SOURCE (or every file when SOURCE is not a
          git work tree). Used to take a buildable copy of the PS5_Vulkan
          checkout before building it outside the shared directory.
  driver  the artefacts PS5_RetroArch@18dc105 links for Vulkan
          (tools/build-title.sh + tools/build.sh, see gpu/vulkan/LINK_CONTRACT.md)
          plus the headers and provenance files that describe them, read from
          a PS5_Vulkan *build root* (the directory holding build/ and .deps/).
  paths   only what --path names.

Extra --path REL entries (file or directory, relative to SOURCE) are added to
any profile.

For every file: sha256 before copy, sha256 of the copy, sha256 after copy.
Any difference (the source changed during the copy) is a hard failure
(exit 3) and the manifest is not written as valid.

Outputs in DEST: the copied files, MANIFEST.json and SHA256SUMS.

Exit codes: 0 ok, 2 usage / missing required input, 3 source changed.
"""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

# Relative to a PS5_Vulkan build root. "required" mirrors the four archives
# PS5_RetroArch's tools/build-title.sh refuses to link without.
DRIVER_REQUIRED = [
    "build/driver/ps5/libps5vk.ps5.a",
    ".deps/native/vulkan-runtime/lib/libvk_runtime.ps5.a",
    "build/driver/ps5/libpsbc_driver.ps5.a",
    ".deps/native/psbc/lib/libpsbc_support.ps5.a",
]
DRIVER_OPTIONAL = [
    # dlopen-style delivery (RetroArch copies it beside eboot.bin); not linked.
    "build/driver/ps5/libvulkan.so.1",
    "build/driver/ps5/libvulkan.so.1.elf",
    # Unrenamed compiler archive and its provenance.
    ".deps/native/psbc/lib/libpsbc.ps5.a",
    ".deps/native/psbc/PROVENANCE.txt",
    ".deps/native/psbc/include/psbc_compile.h",
    ".deps/native/psbc/include/ps5_agc_package.h",
    ".deps/native/vulkan-runtime/PROVENANCE.txt",
    "build/driver/generated/ps5vk_entrypoints.h",
    "build/driver/generated/ps5vk_cache_build.h",
    "build/driver/ps5/flags",
    "build/driver/build-ps5.log",
    # Mesa utility objects (PS5_RetroArch tools/build-mesa-util.sh equivalent,
    # built by gpu/vulkan/ps5/build-mesa-util.sh into this location).
    "build/rcomp-mesa-util/u_thread.o",
    "build/rcomp-mesa-util/anon_file.o",
    "build/rcomp-mesa-util/os_file.o",
]
DRIVER_DIRS = [
    # The Vulkan headers the driver itself was compiled against.
    ".deps/work/psbc-ps5/third_party/Vulkan-Headers/include",
]


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(source: pathlib.Path, *args: str):
    try:
        return subprocess.run(["git", "-C", str(source), *args], check=True,
                              capture_output=True, text=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def list_tree(source: pathlib.Path):
    out = git(source, "ls-files", "-z")
    if out is not None and git(source, "rev-parse", "--show-toplevel"):
        top = pathlib.Path(git(source, "rev-parse", "--show-toplevel").strip())
        if top.resolve() == source.resolve():
            return [p for p in out.split("\0") if p and (source / p).is_file()]
    return sorted(str(p.relative_to(source)) for p in source.rglob("*")
                  if p.is_file() and ".git" not in p.relative_to(source).parts)


def expand(source: pathlib.Path, rel: str):
    p = source / rel
    if p.is_dir():
        return sorted(str(q.relative_to(source)) for q in p.rglob("*") if q.is_file())
    return [rel]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True, type=pathlib.Path)
    ap.add_argument("--dest", required=True, type=pathlib.Path)
    ap.add_argument("--profile", choices=("tree", "driver", "paths"), default="driver")
    ap.add_argument("--path", action="append", default=[], help="extra REL path (file or dir)")
    ap.add_argument("--commit", help="source commit when SOURCE is not the git tree itself "
                    "(e.g. a build dir made from a snapshot)")
    ap.add_argument("--commit-from", type=pathlib.Path,
                    help="read commit/dirty state from this git tree instead of SOURCE")
    ap.add_argument("--label", default="")
    ap.add_argument("--force", action="store_true", help="allow a non-empty DEST")
    a = ap.parse_args()

    source = a.source.resolve()
    dest = a.dest.resolve()
    if not source.is_dir():
        print(f"error: no source directory {source}", file=sys.stderr)
        return 2
    if dest.exists() and any(dest.iterdir()) and not a.force:
        print(f"error: {dest} is not empty (use --force)", file=sys.stderr)
        return 2
    if dest == source or source in dest.parents:
        # A dest inside source would be copied into itself by the tree profile.
        if a.profile == "tree":
            print("error: dest inside source", file=sys.stderr)
            return 2

    missing = []
    files = []
    if a.profile == "tree":
        files = list_tree(source)
    elif a.profile == "driver":
        for rel in DRIVER_REQUIRED:
            if (source / rel).is_file():
                files.append(rel)
            else:
                missing.append(rel)
        files += [r for r in DRIVER_OPTIONAL if (source / r).is_file()]
        for d in DRIVER_DIRS:
            if (source / d).is_dir():
                files += expand(source, d)
            else:
                missing.append(d)
    for rel in a.path:
        if not (source / rel).exists():
            missing.append(rel)
        else:
            files += expand(source, rel)
    if missing:
        for m in missing:
            print(f"error: required input missing: {source / m}", file=sys.stderr)
        return 2
    files = sorted(set(files))

    git_src = (a.commit_from or source).resolve()
    commit = a.commit or (git(git_src, "rev-parse", "HEAD") or "").strip() or None
    porcelain = git(git_src, "status", "--porcelain")
    dirty = None if porcelain is None else [l for l in porcelain.splitlines() if l]

    dest.mkdir(parents=True, exist_ok=True)
    entries, changed = [], []
    for rel in files:
        src = source / rel
        before = sha256(src)
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
        copied = sha256(target)
        after = sha256(src)
        if not (before == copied == after):
            changed.append(rel)
        entries.append({"path": rel, "size": target.stat().st_size, "sha256": copied,
                        "mode": oct(src.stat().st_mode & 0o777)})

    # Second pass over every source once all copies are done: catches a file
    # rewritten after its own copy but before the snapshot finished.
    for e in entries:
        if sha256(source / e["path"]) != e["sha256"]:
            changed.append(e["path"])
    manifest = {
        "schema": "rcomp-vulkan-snapshot-v1",
        "label": a.label,
        "profile": a.profile,
        "source": str(source),
        "source_commit": commit,
        "source_git_tree": str(git_src),
        "source_dirty": dirty,
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "file_count": len(entries),
        "total_bytes": sum(e["size"] for e in entries),
        "files": entries,
        "valid": not changed,
        "changed_during_copy": sorted(set(changed)),
    }
    (dest / "MANIFEST.json").write_text(json.dumps(manifest, indent=1) + "\n")
    (dest / "SHA256SUMS").write_text("".join(f"{e['sha256']}  {e['path']}\n" for e in entries))
    if changed:
        for c in sorted(set(changed)):
            print(f"error: source changed during snapshot: {c}", file=sys.stderr)
        return 3
    print(f"snapshot: {len(entries)} files, {manifest['total_bytes']} bytes, "
          f"commit {commit}, dirty {'unknown' if dirty is None else len(dirty)} -> {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
