#!/usr/bin/env python3
"""Extract one Xbox 360 disc image (XDVDFS, XGD2/XGD3) into the local catalog.

  python3 tools/iso_extract.py /path/to/own.iso [--catalog build/catalog] [--keep-disc]

The image is identified by its SHA-256; everything goes to
build/catalog/<sha256[:16]>/:
  disc/       the extracted files (game content: local only, never shared);
              without --keep-disc only the executable modules (*.xex, *.xexp)
              are kept once the listing has been verified, so the catalog
              stays bounded (the full tree is regenerable from the image).
  disc.json   metadata: image hashes and size, disc layout, file counts,
              default.xex identity from its plaintext XEX headers (title ID,
              media ID, disc number/count, import libraries), secondary
              modules and flags. The image file name stays in this local file.
  extract.log the extractor's output (local only: it lists the disc's files).

The extractor is extract-xiso, built from its pinned source by
tools/build_extract_xiso.sh (deps/deps.lock). Free space is checked before the
extraction. A failure is recorded in disc.json as FAIL with its exact reason;
nothing is skipped silently. Never modifies the image.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
EXTRACTOR = BUILD / "catalog-tools" / "extract-xiso" / "extract-xiso.exe"
EXTRACTOR_COMMIT = "3f5b62cfe68f000b0e3c8a30104973f3a297948e"
SCHEMA = 1
# Offsets of the XDVDFS game partition, as extract-xiso probes them.
LAYOUTS = ((0x0, "game-partition"), (0x0FD90000, "XGD2"), (0x02080000, "XGD3"), (0x18300000, "XGD1"))
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
MODULE_FLAGS = ((0x01, "title"), (0x02, "exports_to_title"), (0x04, "system_debugger"), (0x08, "dll"),
                (0x10, "patch"), (0x20, "patch_full"), (0x40, "patch_delta"), (0x80, "user_mode"))
MODULE_SUFFIXES = (".xex", ".xexp")
MARGIN = 1 << 30  # free space kept beyond the extracted bytes


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def library_version(word):
    """XEX import library version word: major.minor.build.qfe."""
    return f"{word >> 28}.{(word >> 24) & 0xF}.{(word >> 8) & 0xFFFF}.{word & 0xFF}"


def parse_xex_header(data):
    """Identity fields of an XEX2 file from its plaintext header (no decryption).

    Raises ValueError on a malformed header; returns {"magic": ...} only for a
    non-XEX2 magic so the caller can report it.
    """
    if len(data) < 24:
        raise ValueError("truncated XEX header")
    magic = data[:4]
    if magic != b"XEX2":
        return {"magic": magic.hex(), "supported": False}
    flags, header_size, security_off, count = be32(data, 4), be32(data, 8), be32(data, 16), be32(data, 20)
    if header_size > len(data) or 24 + 8 * count > header_size or security_off + 8 > header_size:
        raise ValueError("truncated XEX optional-header table")
    opt = {}
    for i in range(count):
        key, value = be32(data, 24 + 8 * i), be32(data, 28 + 8 * i)
        if key in opt:
            raise ValueError(f"duplicate XEX optional header 0x{key:08X}")
        opt[key] = value
    out = {"magic": "XEX2", "supported": True, "module_flags": f"0x{flags:08X}",
           "module_kinds": [name for bit, name in MODULE_FLAGS if flags & bit],
           "header_size": header_size, "image_size": be32(data, security_off + 4),
           "optional_headers": sorted(f"0x{k:08X}" for k in opt), "execution_info": None,
           "import_libraries": [], "original_pe_name": None}
    if 0x40006 in opt:
        o = opt[0x40006]
        if o < 24 + 8 * count or o + 24 > header_size:
            raise ValueError("truncated XEX execution info header")
        media, version, base_version, title, platform, table, disc, discs, save = struct.unpack_from(">4I4BI", data, o)
        out["execution_info"] = {"media_id": f"0x{media:08X}", "version": f"0x{version:08X}",
                                 "base_version": f"0x{base_version:08X}", "title_id": f"0x{title:08X}",
                                 "platform": platform, "executable_table": table, "disc_number": disc,
                                 "disc_count": discs, "savegame_id": f"0x{save:08X}"}
    if 0x183FF in opt:
        o = opt[0x183FF]
        if o + 4 > header_size or o + be32(data, o) > header_size:
            raise ValueError("truncated XEX original PE name")
        out["original_pe_name"] = data[o + 4:o + be32(data, o)].split(b"\0")[0].decode("latin-1")
    if 0x103FF in opt:
        h = opt[0x103FF]
        if h + 12 > header_size:
            raise ValueError("truncated XEX import table")
        strtab_len, nlibs = be32(data, h + 4), be32(data, h + 8)
        if h + 12 + strtab_len > header_size:
            raise ValueError("truncated XEX import string table")
        names, off = [], 0
        while len(names) < nlibs:
            start = h + 12 + off
            end = data.index(b"\0", start, h + 12 + strtab_len)
            names.append(data[start:end].decode("latin-1"))
            off += (end - start + 1 + 3) & ~3
        lib = h + 12 + strtab_len
        for _ in range(nlibs):
            if lib + 40 > header_size:
                raise ValueError("truncated XEX import library record")
            size, version, minimum = be32(data, lib), be32(data, lib + 0x1C), be32(data, lib + 0x20)
            name_index, records = be16(data, lib + 0x24), be16(data, lib + 0x26)
            if size < 40 or name_index >= len(names):
                raise ValueError("invalid XEX import library record")
            out["import_libraries"].append({"name": names[name_index], "version": library_version(version),
                                            "min_version": library_version(minimum), "records": records})
            lib += size
    return out


def disc_layout(iso):
    with open(iso, "rb") as f:
        for offset, name in LAYOUTS:
            f.seek(offset + 0x10000)
            if f.read(len(XDVDFS_MAGIC)) == XDVDFS_MAGIC:
                return {"layout": name, "partition_offset": f"0x{offset:08X}"}
    return None


def hash_file(path, *algorithms):
    hashes = [hashlib.new(a) for a in algorithms]
    with open(path, "rb") as f:
        while chunk := f.read(8 << 20):
            for h in hashes:
                h.update(chunk)
    return [h.hexdigest() for h in hashes]


def parse_listing(text):
    """extract-xiso -l output -> ({relative path: size}, directory count, declared files, declared bytes)."""
    files, dirs, declared = {}, 0, None
    for line in text.splitlines():
        m = re.match(r"^(.*) \((\d+) bytes\)(?: \[OK\])?$", line.strip())
        if m:
            path = m.group(1).replace("\\", "/").lstrip("/")
            if path.endswith("/"):
                dirs += 1
            else:
                files[path] = int(m.group(2))
            continue
        m = re.match(r"^(\d+) files in .* total (\d+) bytes$", line.strip())
        if m:
            declared = (int(m.group(1)), int(m.group(2)))
    return files, dirs, declared


def inside(path, parent):
    try:
        return Path(path).resolve().is_relative_to(Path(parent).resolve())
    except OSError:
        return False


def module_record(disc, rel):
    path = disc / rel
    size = path.stat().st_size
    with open(path, "rb") as f:
        head = f.read(24)
        if len(head) == 24 and head[:4] == b"XEX2":
            head += f.read(max(0, min(be32(head, 8), size) - 24))  # the plaintext header only
    rec = {"path": rel, "size": size, "sha256": hash_file(path, "sha256")[0]}
    try:
        rec["header"] = parse_xex_header(head)
    except (ValueError, struct.error) as error:
        rec["header"] = None
        rec["header_error"] = str(error)
    return rec


def flags_for(report):
    xex = report.get("default_xex") or {}
    info = ((xex.get("header") or {}).get("execution_info")) or {}
    discs = info.get("disc_count") or 0
    secondary = report.get("modules") or []
    reasons = []
    if not xex.get("present"):
        reasons.append("no default.xex on the disc")
    return {
        "multi_disc": discs > 1,
        "disc_number": info.get("disc_number"),
        "disc_count": info.get("disc_count"),
        "install_disc_suspected": bool(reasons),
        "install_disc_reasons": reasons,
        "secondary_modules": len(secondary),
        "patch_modules": sum(1 for m in secondary if "patch" in ((m.get("header") or {}).get("module_kinds") or [])),
        "dll_modules": sum(1 for m in secondary if "dll" in ((m.get("header") or {}).get("module_kinds") or [])),
    }


def prune(disc, keep):
    """Remove every extracted file but the kept modules. Only inside our own marked tree."""
    if not (disc / ".rcomp-extracted").is_file():
        raise ValueError("refusing to prune a tree this tool did not create")
    for path in sorted(disc.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        rel = path.relative_to(disc).as_posix()
        if path.is_symlink():
            raise ValueError("link inside extracted tree: " + rel)
        if path.is_file() and rel not in keep and rel != ".rcomp-extracted":
            path.unlink()
        elif path.is_dir() and not any(path.iterdir()):
            path.rmdir()


def extract(iso, catalog, extractor, keep_disc, force):
    started = time.time()
    iso = Path(iso).resolve()
    report = {"tool": "tools/iso_extract.py", "schema": SCHEMA, "status": "FAIL", "reason": None,
              "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}
    if not iso.is_file():
        raise SystemExit(f"FAIL: {iso}: not a file")
    size = iso.stat().st_size
    sha256, sha1 = hash_file(iso, "sha256", "sha1")
    ident = sha256[:16]
    out = Path(catalog) / ident
    report["id"] = ident
    report["iso"] = {"path": str(iso), "file_name": iso.name, "size": size, "sha256": sha256, "sha1": sha1}
    previous = out / "disc.json"
    if previous.is_file() and not force:
        old = json.loads(previous.read_text(encoding="utf-8"))
        if old.get("iso", {}).get("sha256") == sha256 and old.get("status") == "PASS":
            print(f"{ident}: already extracted (PASS); --force to redo")
            return old
    out.mkdir(parents=True, exist_ok=True)
    disc = out / "disc"

    def finish(status, reason=None):
        report["status"], report["reason"] = status, reason
        report["seconds"] = round(time.time() - started, 1)
        report["flags"] = flags_for(report)
        previous.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
        print(f"{ident}: {status}" + (f": {reason}" if reason else ""))
        return report

    extractor = Path(extractor).resolve()
    if not extractor.is_file():
        return finish("FAIL", f"extractor missing: {extractor} (run tools/build_extract_xiso.sh)")
    report["extractor"] = {"path": str(extractor), "sha256": hash_file(extractor, "sha256")[0],
                           "source_commit": EXTRACTOR_COMMIT}
    report["disc"] = disc_layout(iso)
    if report["disc"] is None:
        return finish("FAIL", "no XDVDFS volume descriptor at any known partition offset")
    listing = subprocess.run([str(extractor), "-l", str(iso)], capture_output=True, text=True, errors="replace")
    files, dirs, declared = parse_listing(listing.stdout)
    total = sum(files.values())
    report["listing"] = {"exit_code": listing.returncode, "files": len(files), "directories": dirs, "bytes": total,
                         "declared": {"files": declared[0], "bytes": declared[1]} if declared else None}
    if listing.returncode != 0 or declared is None:
        return finish("FAIL", "extract-xiso -l failed: " + (listing.stderr or listing.stdout).strip()[-400:])
    if declared != (len(files), total):
        return finish("FAIL", f"listing parse mismatch: parsed {len(files)} files/{total} bytes, "
                              f"declared {declared[0]}/{declared[1]}")
    if disc.exists():
        if not (disc / ".rcomp-extracted").is_file() or not inside(disc, BUILD):
            return finish("FAIL", f"{disc} exists and was not created by this tool")
        shutil.rmtree(disc)
    free = shutil.disk_usage(out).free
    report["free_space"] = {"before": free, "required": total + MARGIN}
    if free < total + MARGIN:
        return finish("FAIL", f"insufficient free space: {free} bytes free, {total + MARGIN} required")
    disc.mkdir()
    (disc / ".rcomp-extracted").write_text(sha256 + "\n")
    t = time.time()
    with open(out / "extract.log", "w", encoding="utf-8", errors="replace") as log:
        rc = subprocess.run([str(extractor), "-x", "-d", str(disc), str(iso)], stdout=log,
                            stderr=subprocess.STDOUT).returncode
    report["extraction"] = {"exit_code": rc, "seconds": round(time.time() - t, 1)}
    got = {p.relative_to(disc).as_posix(): p.stat().st_size for p in disc.rglob("*")
           if p.is_file() and p.name != ".rcomp-extracted"}
    report["extraction"].update({"files": len(got), "bytes": sum(got.values())})
    mismatched = sorted(set(files) ^ set(got)) + sorted(p for p in files if p in got and files[p] != got[p])
    if rc != 0:
        tail = (out / "extract.log").read_text(errors="replace").strip().splitlines()[-3:]
        return finish("FAIL", f"extract-xiso -x exited {rc}: " + " | ".join(tail))
    if mismatched:
        return finish("FAIL", f"extracted tree differs from the listing in {len(mismatched)} paths "
                              f"(first: {mismatched[0]})")
    report["extraction"]["verified"] = True
    modules = sorted(p for p in got if p.lower().endswith(MODULE_SUFFIXES))
    default = next((p for p in modules if p.lower() == "default.xex"), None)
    report["default_xex"] = {"present": default is not None}
    if default:
        report["default_xex"].update(module_record(disc, default))
    report["modules"] = [module_record(disc, p) for p in modules if p != default]
    report["top_level"] = sorted({p.split("/")[0] for p in got})
    report["title_update_on_disc"] = any(p.lower().startswith(("$titleupdate/", "update/")) for p in got)
    if not keep_disc:
        prune(disc, set(modules))
    report["extraction"]["pruned"] = not keep_disc
    header = (report["default_xex"].get("header") or {}) if default else {}
    if default and not header.get("supported"):
        return finish("FAIL", "default.xex header unreadable: " +
                      report["default_xex"].get("header_error", f"magic {header.get('magic')}"))
    return finish("PASS")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso", nargs="+")
    ap.add_argument("--catalog", default=str(BUILD / "catalog"))
    ap.add_argument("--extractor", default=str(EXTRACTOR))
    ap.add_argument("--keep-disc", action="store_true", help="keep the whole extracted tree (default: modules only)")
    ap.add_argument("--force", action="store_true", help="redo an image already extracted with PASS")
    a = ap.parse_args()
    if not inside(a.catalog, BUILD):
        ap.error("the catalog must stay under this repository's build/ (game content)")
    failed = 0
    for iso in a.iso:  # one image at a time: disk usage stays bounded by the largest image
        report = extract(iso, a.catalog, a.extractor, a.keep_disc, a.force)
        failed += report["status"] != "PASS"
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
