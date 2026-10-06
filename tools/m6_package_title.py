#!/usr/bin/env python3
"""Assemble a local title folder. Does not validate SELF/ELF or run PS5 code."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
from m6_inventory import output_allowed


def no_links(path):
    path = Path(path).absolute()
    for part in (path, *path.parents):
        if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
            raise ValueError("symlink/junction is not allowed: " + str(part))
    return path.resolve()


def source_file(path):
    path = no_links(path)
    if not path.is_file():
        raise ValueError("missing regular file: " + str(path))
    return path


def tree_files(root):
    root = no_links(root)
    if not root.is_dir():
        raise ValueError("missing source directory: " + str(root))
    files = []
    for path in sorted(root.rglob("*")):
        no_links(path)
        if path.is_file():
            files.append((path, path.relative_to(root)))
        elif not path.is_dir():
            raise ValueError("non-regular source entry: " + str(path))
    return files


def package_title(linked, image, game_root, template, destination):
    """Copy validated inputs to a NEW directory; return assembly manifest."""
    linked, game_root, template, destination = map(no_links, (linked, game_root, template, destination))
    image = source_file(image)
    if not output_allowed(str(destination)):
        raise ValueError("destination must be outside git worktrees or under build/")
    if destination.exists():
        raise ValueError("destination already exists; use a fresh directory")
    for source in (linked, image, game_root, template):
        if destination.is_relative_to(source) or source.is_relative_to(destination):
            raise ValueError("destination overlaps an input: " + str(source))
    inputs = [(source_file(linked / name), Path(name)) for name in ("eboot.bin", "eboot.elf")]
    inputs.append((image, Path("image/plain.xex")))
    for source, _ in inputs:
        if source.stat().st_size == 0:
            raise ValueError("empty required artifact: " + str(source))
    param_file = source_file(template / "sce_sys/param.json")
    source_file(template / "sce_module/libc.prx")
    if (template / "sce_module/libc.prx").stat().st_size == 0:
        raise ValueError("empty libc.prx")
    param = json.loads(param_file.read_text(encoding="utf-8"))
    if not isinstance(param, dict):
        raise ValueError("param.json must be an object")
    title_id = param.get("titleId", "")
    if not isinstance(title_id, str) or not re.fullmatch(r"PPSA\d{5}", title_id):
        raise ValueError("invalid titleId")
    if not isinstance(param.get("contentId"), str) or not re.fullmatch(
            r"[A-Z]{2}\d{4}-" + title_id + r"_00-[A-Z0-9]{16}", param["contentId"]):
        raise ValueError("contentId does not match titleId")
    if not isinstance(param.get("conceptId"), str) or not re.fullmatch(r"\d{5}", param["conceptId"]):
        raise ValueError("invalid conceptId")
    for name in ("sce_sys", "sce_module"):
        inputs.extend((source, Path(name) / relative) for source, relative in tree_files(template / name))
    inputs.extend((source, Path("game") / relative) for source, relative in tree_files(game_root))
    targets = [relative.as_posix().casefold() for _, relative in inputs]
    if len(targets) != len(set(targets)):
        raise ValueError("case-insensitive collision among output files")
    destination.mkdir(parents=True)
    marker = destination / "ASSEMBLY_INCOMPLETE"
    marker.write_text("Assembly did not complete. Do not deploy.\n", encoding="utf-8")
    records = {}
    for source, relative in inputs:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with source.open("rb") as src, target.open("xb") as dst:
            shutil.copyfileobj(src, dst)
        with target.open("rb") as copied:
            digest = hashlib.file_digest(copied, "sha256").hexdigest()
        records[relative.as_posix()] = {"bytes": target.stat().st_size, "sha256": digest}
    manifest = {"schema": 1, "status": "PASS", "scope": "title folder assembly only",
                "title_id": title_id, "ps5_executable_validity": "NOT TESTED",
                "ps5_execution": "NOT TESTED", "gta_iv_boot": "NOT TESTED", "files": records}
    (destination / "ASSEMBLY.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (destination / "manifest.sha256").write_text(
        "".join(f"{record['sha256']}  {name}\n" for name, record in sorted(records.items())), encoding="utf-8")
    marker.unlink()
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("linked", "image", "game-root", "template", "out"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = package_title(args.linked, args.image, args.game_root, args.template, args.out)
    except (OSError, ValueError) as error:
        print("BLOCKED: " + str(error), file=sys.stderr)
        return 2
    print(f"PASS: title folder assembly ({len(manifest['files'])} files): {args.out}")
    print("PS5 executable validity / PS5 execution / GTA IV: NOT TESTED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
