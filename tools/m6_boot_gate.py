#!/usr/bin/env python3
"""Check M6 inventory AND generated artifacts. Passing is not a game-boot claim."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile


def positive_int(value):
    return type(value) is int and value > 0


def static_tls_blockers(image):
    """Verified loader subset; dynamic KeTls slots are a separate mechanism."""
    headers = image.get("optional_headers")
    if not isinstance(headers, list): return ["image optional-header inventory is missing"]
    present, tls = headers.count("0x00020104"), image.get("static_tls")
    if not present:
        return [] if tls is None else ["static TLS metadata exists without its optional header"]
    if present != 1: return ["duplicate XEX static TLS headers"]
    fields = ("slot_count", "raw_data_address", "data_size", "raw_data_size")
    if not isinstance(tls, dict) or any(type(tls.get(field)) is not int or not 0 <= tls[field] <= 0xFFFFFFFF for field in fields):
        return ["XEX static TLS metadata is missing or invalid"]
    slots, address, size, raw = (tls[field] for field in fields)
    errors = []
    if raw > size: errors.append("XEX static TLS template exceeds data_size")
    if not slots and size: errors.append("XEX static TLS nonempty data with zero slots is unsupported")
    if size + 4 * slots > 16 * 1024 * 1024:
        errors.append("XEX static TLS allocation exceeds runtime 16 MiB resource limit")
    if raw:
        try:
            base, image_size = int(image["base"], 16), image["size"]
            valid_image = type(image_size) is int and image_size > 0 and 0 <= base < 2**32 and base + image_size <= 2**32
        except (KeyError, TypeError, ValueError): valid_image = False
        if not valid_image or address == 0 or address < base or address + raw > base + image_size:
            errors.append("XEX static TLS template is outside decoded image")
    return errors


def artifact_snapshot(work):
    """Bind a report to exact inputs/sources; never follow artifact links."""
    work = Path(work)
    ppc = work / "ppc"
    paths = [work / name for name in ("plain.xex", "game.toml", "switch_tables.toml")]
    for root in (work, ppc):
        if root.is_symlink() or (hasattr(root, "is_junction") and root.is_junction()) or not root.is_dir():
            raise ValueError("missing or unsafe generated source directory")
    for path in ppc.rglob("*"):
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            raise ValueError("linked generated artifact: " + str(path))
        if path.is_file(): paths.append(path)
        elif not path.is_dir(): raise ValueError("non-regular generated artifact: " + str(path))
    records = {}
    for path in paths:
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(work.resolve()):
            raise ValueError("missing or unsafe generated artifact: " + str(path))
        with path.open("rb") as data: digest = hashlib.file_digest(data, "sha256").hexdigest()
        records[path.relative_to(work).as_posix()] = {"bytes": path.stat().st_size, "sha256": digest}
    return records


def check_inventory(report, work):
    """Return blockers without trusting a precomputed supported_for_link flag."""
    errors = []
    work = Path(work)
    if not isinstance(report, dict):
        return ["inventory must be an object"]
    if report.get("supported_for_link") is not True:
        errors.append("inventory did not explicitly permit linking")
    for field in ("decode", "jump_tables", "xenonrecomp", "image", "imports"):
        if not isinstance(report.get(field), dict):
            errors.append("missing or invalid inventory section: " + field)
    if errors and any(not isinstance(report.get(k), dict) for k in
                      ("decode", "jump_tables", "xenonrecomp", "image", "imports")):
        return errors
    dec, jumps, recomp = (report[k] for k in ("decode", "jump_tables", "xenonrecomp"))
    if dec.get("supported") is not True or type(dec.get("compression")) is not int or dec["compression"] not in (0, 1, 2):
        errors.append("decode did not report a supported compression type")
    if type(dec.get("encryption")) is not int or dec["encryption"] not in (0, 1):
        errors.append("decode did not report a supported encryption type")
    if type(jumps.get("xenonanalyse_exit")) is not int or jumps["xenonanalyse_exit"] != 0 or type(jumps.get("tables")) is not int or jumps["tables"] < 0:
        errors.append("jump-table analysis is missing or failed")
    if recomp.get("ran") is not True or type(recomp.get("exit_code")) is not int or recomp["exit_code"] != 0:
        errors.append("successful recompilation is required, not an inventory-only run")
    if not positive_int(recomp.get("functions")) or not positive_int(recomp.get("generated_bytes")):
        errors.append("recompiler reported no usable function table or sources")
    for field in ("warnings", "unrecognized_instructions"):
        if recomp.get(field) != {}:
            errors.append("recompiler diagnostics are missing or nonempty: " + field)
    if report.get("blocking_reasons") != []:
        errors.append("inventory blockers are missing or nonempty")
    try:
        actual = artifact_snapshot(work)
        if report.get("artifacts") != actual:
            errors.append("artifact hashes/sizes disagree with inventory or are missing")
        generated = {name: entry for name, entry in actual.items() if name.startswith("ppc/")}
        if sum(entry["bytes"] for entry in generated.values()) != recomp.get("generated_bytes"):
            errors.append("generated byte count disagrees with files")
        if not any(name.endswith(".cpp") and name != "ppc/ppc_func_mapping.cpp" and entry["bytes"] > 0
                   for name, entry in generated.items()):
            errors.append("no generated implementation source beyond the function mapping")
    except (OSError, ValueError) as error: errors.append(str(error))
    for module, imported in report["imports"].items():
        if not isinstance(imported, dict):
            errors.append("invalid import report: " + module)
            continue
        for field in ("functions_missing", "variables_missing", "unknown_ordinals"):
            if imported.get(field) != []:
                errors.append(module + ": " + field + " is missing or nonempty")
    # Fresh inventories bind source-level support to the actual runtime build
    # selection. Existing archived inventories retain their historical schema.
    selection = report.get("runtime_source_selection")
    if selection is not None:
        from runtime_source_inventory import source_imports
        root = Path(__file__).resolve().parents[1]
        try:
            _, _, units = source_imports(root)
            paths = {"runtime/CMakeLists.txt", "app/src/title_runtime.cpp", *units}
            expected = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                        for name in sorted(paths)}
            if not isinstance(selection, dict) or selection.get("files") != expected:
                errors.append("runtime build/registration sources changed after inventory")
        except (OSError, ValueError) as error:
            errors.append("runtime source selection invalid: " + str(error))
    errors.extend(static_tls_blockers(report["image"]))
    for name in ("plain.xex", "game.toml", "switch_tables.toml", "ppc/ppc_func_mapping.cpp"):
        p = work / name
        if p.is_symlink() or not p.is_file() or not p.resolve().is_relative_to(work.resolve()):
            errors.append("missing or unsafe generated artifact: " + name)
        elif name != "switch_tables.toml" and p.stat().st_size == 0:
            errors.append("empty generated artifact: " + name)
    mapping = work / "ppc/ppc_func_mapping.cpp"
    if mapping.is_file() and not mapping.is_symlink() and mapping.resolve().is_relative_to(work.resolve()):
        addresses = re.findall(r"\{\s*0[xX]([0-9a-fA-F]+)\s*,", mapping.read_text(encoding="utf-8", errors="replace"))
        addresses = [int(a, 16) for a in addresses]
        try:
            entry = int(report["image"]["entry"], 16)
        except (KeyError, TypeError, ValueError):
            entry = None
        if entry is None or entry not in addresses:
            errors.append("entry point is absent from the generated function table")
        if len(addresses) != len(set(addresses)):
            errors.append("duplicate guest addresses in generated function table")
        if not addresses or len(addresses) != recomp.get("functions"):
            errors.append("function-table count disagrees with inventory")
    return errors


def write_gate_report(work, result):
    """Never follow an output link, including when reporting a blocked run."""
    target = Path(work).absolute() / "BOOT_GATE.json"
    under_build = False
    for path in (target, *target.parents):
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            raise ValueError("unsafe gate output link: " + str(path))
        if path.name == "build": under_build = True
        if (path / ".git").exists():
            if not under_build: raise ValueError("gate output must be outside git worktrees or under build/")
            break
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=target.parent,
                                     prefix=".BOOT_GATE-", delete=False) as temp:
        temporary = Path(temp.name)
        temp.write(json.dumps(result, indent=2) + "\n")
    try:
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("work", type=Path)
    ap.add_argument("--xex", type=Path, help="original XEX; records its SHA-256, never its bytes")
    args = ap.parse_args()
    try:
        report = json.loads((args.work / "inventory.json").read_text(encoding="utf-8"))
        errors = check_inventory(report, args.work)
        result = {"schema": 1, "status": "BLOCKED" if errors else "PASS",
                  "scope": "metadata and generated artifact consistency",
                  "gta_iv_boot": "NOT TESTED", "ps5_execution": "NOT TESTED", "blockers": errors}
        if args.xex:
            with args.xex.open("rb") as f:
                result["input_xex_sha256"] = hashlib.file_digest(f, "sha256").hexdigest()
        write_gate_report(args.work, result)
        for error in errors:
            print("BLOCKED: " + error, file=sys.stderr)
        print(result["status"] + ": metadata/artifacts only; PS5 and gameplay NOT TESTED")
        return 1 if errors else 0
    except (OSError, ValueError, TypeError) as exc:
        print("BLOCKED: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
