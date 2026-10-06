#!/usr/bin/env python3
"""M6 inventory of a user's own XEX: what R-comp still lacks to run it.

Runs locally on the machine that holds the (legally owned) title. Linux, WSL
or macOS; the host tools come from tools/m6_setup.sh.

  python3 tools/m6_inventory.py /path/to/default.xex --out build/m6/<name> [--recompile]

Steps: decode (encrypted/compressed -> plain XEX, cpu/tools/xex_decode.cpp) ->
parse headers, sections and import records -> look for the register
save/restore helpers XenonRecomp needs -> XenonAnalyse (jump tables) ->
optionally XenonRecomp, classifying every warning.

Output (in --out, which must be outside any git work tree or under build/):
  INVENTORY.md, inventory.json   metadata only: API names from the export
                                 tables, counts, instruction mnemonics, section
                                 names and sizes. Safe to share.
  plain.xex, ppc/, *.log, *.toml derived from the game: never commit or
                                 publish them.
"""
import argparse
import collections
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
from m6_boot_gate import artifact_snapshot, static_tls_blockers
from runtime_source_inventory import source_imports

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
XENON_SRC = os.path.join(BUILD, "cpu-xenonrecomp-src")
TOOLS = {
    "decode": os.path.join(BUILD, "cpu-xex-decode"),
    "analyse": os.path.join(BUILD, "cpu-xenonrecomp", "XenonAnalyse", "XenonAnalyse"),
    "recomp": os.path.join(BUILD, "cpu-xenonrecomp", "XenonRecomp", "XenonRecomp"),
}
# runtime/src/hle_<module>*.cpp register the implementations of that module.
MODULE_OF_HLE_PREFIX = {"hle_xboxkrnl": "xboxkrnl.exe", "hle_xam": "xam.xex"}
# Boolean keys of the generator's [main] table that change the code it emits (the audit's C2:
# docs/OPTIMIZATION_AUDIT_20260930.md). skip_lr is left out: the prelude and the runtime use PPCContext::lr.
RECOMP_OPTIONS = ("cr_as_local", "ctr_as_local", "xer_as_local", "reserved_as_local",
                  "non_argument_as_local", "non_volatile_as_local")


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def output_allowed(path):
    candidate = Path(path).absolute()
    for part in (candidate, *candidate.parents):
        if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
            return False
    d = str(candidate.resolve())
    under_build = False
    while True:
        if os.path.basename(d) == "build":
            under_build = True
        if os.path.exists(os.path.join(d, ".git")):
            return under_build
        parent = os.path.dirname(d)
        if parent == d:
            return True
        d = parent


def export_tables():
    """{module: {ordinal: (name, kind)}} from XenonRecomp's copy of Xenia's tables."""
    tables = {}
    for module, fn in (("xboxkrnl.exe", "xboxkrnl_table.inc"), ("xam.xex", "xam_table.inc")):
        path = os.path.join(ROOT, "third_party", "XenonRecomp", "XenonUtils", "xbox", fn)
        t = {}
        for m in re.finditer(r"XE_EXPORT\(\s*\w+\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)", Path(path).read_text()):
            t[int(m.group(1), 16)] = (m.group(2), "variable" if m.group(3) == "kVariable" else "function")
        tables[module] = t
    return tables


def implemented_imports():
    """Source-level implementations selected and registered by the runtime build."""
    return source_imports(ROOT)[0]


def implemented_variables():
    """Variables registered by selected production bootstrap source units."""
    return source_imports(ROOT)[1]


def parse_plain_xex(x):
    if x[:4] != b"XEX2":
        sys.exit("not an XEX2 file")
    if len(x) < 24:
        raise ValueError("truncated XEX header")
    header_size, security_off, count = be32(x, 8), be32(x, 16), be32(x, 20)
    if header_size > len(x) or 24 + 8 * count > header_size:
        raise ValueError("truncated XEX optional-header table")
    keys = [be32(x, 24 + 8 * i) for i in range(count)]
    if keys.count(0x20104) > 1:
        raise ValueError("duplicate XEX static TLS header")
    if keys.count(0x40006) > 1:
        raise ValueError("duplicate XEX execution info header")
    opt = {be32(x, 24 + 8 * i): be32(x, 28 + 8 * i) for i in range(count)}
    execution_info = None
    if 0x40006 in opt:
        offset = opt[0x40006]
        if offset < 24 + 8 * count or offset + 24 > header_size:
            raise ValueError("truncated or overlapping XEX execution info header")
        # Xenia @95a5c3e, kernel/util/xex2_info.h: xex2_opt_execution_info.
        # Preserve version words exactly; do not infer an edition from filenames.
        fields = struct.unpack_from(">4I4BI", x, offset)
        names = ("media_id", "version", "base_version", "title_id", "platform",
                 "executable_table", "disc_number", "disc_count", "savegame_id")
        execution_info = {name: (f"0x{value:08X}" if index < 4 or index == 8 else value)
                          for index, (name, value) in enumerate(zip(names, fields))}
    static_tls = None
    if 0x20104 in opt:
        offset = opt[0x20104]
        if offset < 24 + 8 * count or offset + 16 > header_size:
            raise ValueError("truncated or overlapping XEX static TLS header")
        static_tls = dict(zip(("slot_count", "raw_data_address", "data_size", "raw_data_size"),
                              struct.unpack_from(">4I", x, offset)))
    image_size = be32(x, security_off + 4)
    if security_off + 0x114 > header_size or image_size == 0 or header_size + image_size > len(x):
        raise ValueError("truncated XEX security header or image")
    base = opt.get(0x10201, be32(x, security_off + 4 + 4 + 0x100 + 4 + 4))
    entry = opt.get(0x10100, 0)
    image = x[header_size:header_size + image_size]
    # PE sections (little-endian headers inside the image).
    e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    nsec = struct.unpack_from("<H", image, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", image, e_lfanew + 20)[0]
    sections = []
    for i in range(nsec):
        o = e_lfanew + 24 + opt_size + 40 * i
        name = image[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsize, va = struct.unpack_from("<II", image, o + 8)
        flags = struct.unpack_from("<I", image, o + 36)[0]
        sections.append({"name": name, "va": base + va, "size": vsize, "code": bool(flags & 0x20)})
    # Import libraries.
    imports = []
    if 0x103FF in opt:
        h = opt[0x103FF]
        strtab_len, nlibs = be32(x, h + 4), be32(x, h + 8)
        names, off = [], 0
        while len(names) < nlibs:
            end = x.index(b"\0", h + 12 + off)
            s = x[h + 12 + off:end].decode("latin-1")
            names.append(s)
            off += (len(s) + 1 + 3) & ~3
        lib = h + 12 + strtab_len
        for _ in range(nlibs):
            size, name_index, n = be32(x, lib), be16(x, lib + 0x24), be16(x, lib + 0x26)
            for k in range(n):
                va = be32(x, lib + 40 + 4 * k)
                word = be32(image, va - base)
                imports.append({"module": names[name_index], "type": word >> 24, "ordinal": word & 0xFFFF})
            lib += size
    info = {"base": base, "image_size": image_size, "entry": entry, "sections": sections,
            "optional_headers": sorted(f"0x{k:08X}" for k in opt), "static_tls": static_tls,
            "execution_info": execution_info}
    return info, image, imports


# XEX2 module flags (Xenia @95a5c3e, kernel/util/xex2_info.h: xex2_module_flags).
XEX_MODULE_DLL = 0x08
XEX_MODULE_PATCH = 0x10 | 0x20 | 0x40
# Disc folders that never hold title code (the console's own system update).
NON_TITLE_DIRS = {"$systemupdate"}


def xex_module_flags(x):
    return be32(x, 4)


def xex_original_name(x):
    """XEX_HEADER_ORIGINAL_PE_NAME (0x000183FF), or None."""
    header_size, count = be32(x, 8), be32(x, 20)
    for i in range(count):
        if be32(x, 24 + 8 * i) == 0x183FF:
            offset = be32(x, 28 + 8 * i)
            if offset + 4 > header_size:
                raise ValueError("truncated XEX original name header")
            size = be32(x, offset)
            if size < 4 or offset + size > header_size:
                raise ValueError("truncated XEX original name header")
            return x[offset + 4:offset + size].split(b"\0", 1)[0].decode("latin-1") or None
    return None


def parse_exports(x, info, image):
    """{ordinal: guest address} a loader resolves for this module.

    The XEX export table named by the security info (Xenia: XexModule::GetProcAddress,
    xex2_export_table: ordOffset[ordinal - base] + (imagebaseaddr << 16); a zero offset
    exports nothing), else the PE export directory of the image."""
    base = info["base"]
    security = be32(x, 16)
    table = be32(x, security + 0x160) if security + 0x164 <= be32(x, 8) else 0
    exports = {}
    if table:
        offset = table - base
        if offset < 0 or offset + 0x2C > len(image):
            raise ValueError("XEX export table outside the image")
        image_base, count, first = struct.unpack_from(">3I", image, offset + 0x20)
        if offset + 0x2C + 4 * count > len(image):
            raise ValueError("truncated XEX export table")
        for index, ord_offset in enumerate(struct.unpack_from(f">{count}I", image, offset + 0x2C)):
            if ord_offset:
                exports[first + index] = (ord_offset + (image_base << 16)) & 0xFFFFFFFF
        return exports, "xex"
    e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    magic = struct.unpack_from("<H", image, e_lfanew + 24)[0]
    directories = e_lfanew + 24 + (96 if magic == 0x10B else 112)
    rva, size = struct.unpack_from("<II", image, directories)
    if not rva or not size:
        return {}, "none"
    if rva + 40 > len(image):
        raise ValueError("PE export directory outside the image")
    first, count = struct.unpack_from("<II", image, rva + 16)
    functions = struct.unpack_from("<I", image, rva + 28)[0]
    if functions + 4 * count > len(image):
        raise ValueError("truncated PE export directory")
    for index, address in enumerate(struct.unpack_from(f"<{count}I", image, functions)):
        if address:
            exports[first + index] = (base + address) & 0xFFFFFFFF
    return exports, "pe"


def find_disc_modules(disc_root, main_xex, exclude):
    """Every XEX2 file under the disc root besides the main executable (by magic, any
    extension), sorted by disc path; folders that never hold title code are skipped, and so
    is `exclude` (this run's output, when it lies under the disc root)."""
    found, skipped = [], []
    main, exclude = Path(main_xex).resolve(), Path(exclude).resolve()
    for directory, folders, files in os.walk(disc_root):
        for folder in sorted(folders):
            if folder.lower() in NON_TITLE_DIRS:
                skipped.append(Path(directory, folder).relative_to(disc_root).as_posix())
        folders[:] = sorted(f for f in folders if f.lower() not in NON_TITLE_DIRS
                            and not Path(directory, f).is_symlink() and Path(directory, f).resolve() != exclude)
        for name in sorted(files):
            path = Path(directory, name)
            if path.is_symlink() or path.resolve() == main:
                continue
            try:
                with open(path, "rb") as f:
                    magic = f.read(4)
            except OSError:
                continue
            if magic == b"XEX2":
                found.append(path)
    return found, skipped


def module_key(disc_path, used):
    """C identifier for a module's symbols, unique within the title."""
    key = re.sub(r"[^A-Za-z0-9]", "_", disc_path)
    candidate, n = key, 1
    while candidate.lower() in used:
        n += 1
        candidate = f"{key}_{n}"
    used.add(candidate.lower())
    return candidate


def read_mapping(path):
    """[(guest address, symbol)] of a generated ppc_func_mapping.cpp, in table order."""
    pattern = re.compile(r"^\t\{ 0x([0-9A-F]+), ([A-Za-z_][A-Za-z0-9_]*) \},$")
    entries = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = pattern.match(line)
        if m:
            entries.append((int(m.group(1), 16), m.group(2)))
    return entries


def write_module_descriptor(module_dir, key, disc_path, entries):
    """aot_module.cpp: the module's (guest -> host) table as rcomp::FuncEntry (rcomp/aot_modules.h)."""
    lines = [f"// Generated by tools/m6_inventory.py for the AOT module {disc_path}; never edit.",
             '#include "ppc/ppc_recomp_shared.h"', '#include "rcomp/aot_modules.h"', "",
             f"extern const rcomp::FuncEntry rcomp_aot_functions_{key}[] = {{"]
    lines += [f"    {{0x{address:08X}u, {name}, nullptr}}," for address, name in entries]
    lines += ["};", ""]
    Path(module_dir, "aot_module.cpp").write_text("\n".join(lines), encoding="utf-8")


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_module_list(out, modules):
    """aot_modules.cpp: the descriptors app/m6/main.cpp passes to rcomp::register_aot_modules."""
    lines = ["// Generated by tools/m6_inventory.py: the AOT modules of this title; never edit.",
             '#include "rcomp/aot_modules.h"', ""]
    lines += [f"extern const rcomp::FuncEntry rcomp_aot_functions_{m['key']}[];" for m in modules]
    lines += ["", "extern const rcomp::AotModule rcomp_m6_aot_modules[] = {"]
    for m in modules:
        lines.append(f"    {{{c_string(m['disc_path'])}, {c_string(m['module_name'])}, {m['image']['base']}u, "
                     f"0x{m['image']['size']:X}u, {m['image']['entry']}u, rcomp_aot_functions_{m['key']}, "
                     f"{m['functions']}u}},")
    lines += ["};", f"extern const size_t rcomp_m6_aot_module_count = {len(modules)};", ""]
    Path(out, "aot_modules.cpp").write_text("\n".join(lines), encoding="utf-8")


def module_import_records(records, providers):
    """{library: {provider, ordinals, thunks}} for the import records naming an AOT module."""
    found = collections.OrderedDict()
    for r in records:
        provider = providers.get(r["module"].lower())
        if provider is None:
            continue
        d = found.setdefault(r["module"], {"provider": provider, "ordinals": set(), "thunks": set()})
        d["ordinals"].add(r["ordinal"])
        if r["type"] != 0:
            d["thunks"].add(r["ordinal"])
    return found


def verify_module_imports(found, functions_by_module):
    """Every imported ordinal is exported by the providing module, and the export behind each
    function thunk is a function the provider's generated code defines."""
    report, blockers = {}, []
    for library, d in found.items():
        provider = d["provider"]
        exports = provider["_exports"]
        functions = functions_by_module.get(provider["disc_path"])
        missing = sorted(o for o in d["ordinals"] if o not in exports)
        not_functions = sorted(o for o in d["thunks"] if o in exports and functions is not None
                               and exports[o] not in functions)
        report[library] = {"provider": provider["disc_path"], "ordinals": len(d["ordinals"]),
                           "function_thunks": len(d["thunks"]),
                           "missing_exports": [f"0x{o:04X}" for o in missing],
                           "exports_without_function": [f"0x{o:04X}" for o in not_functions]}
        if missing:
            blockers.append(f"imports from {library} that {provider['disc_path']} does not export: "
                            + ", ".join(report[library]["missing_exports"]))
        if not_functions:
            blockers.append(f"function imports from {library} whose export is no recompiled function: "
                            + ", ".join(report[library]["exports_without_function"]))
        if functions is None and d["thunks"]:
            blockers.append(f"function imports from {library}: {provider['disc_path']} was not recompiled")
    return report, blockers


def toml_string_list(values):
    return "[" + ", ".join(json.dumps(v) for v in values) + "]"


def prepare_module(path, disc_root, out, used_keys, tools, timeout):
    """Decodes and parses one candidate module. Returns its report entry; internal data
    (keys starting with '_') is dropped before the report is written."""
    disc_path = path.relative_to(disc_root).as_posix()
    raw = path.read_bytes()
    if len(raw) < 24:
        return {"disc_path": disc_path, "reason": "truncated XEX2 header: not compiled", "blocking_reasons": []}
    entry = {"disc_path": disc_path, "module_flags": f"0x{xex_module_flags(raw):08X}",
             "source_xex": {"size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()},
             "blocking_reasons": []}
    flags = xex_module_flags(raw)
    if not flags & XEX_MODULE_DLL or flags & XEX_MODULE_PATCH:
        entry["reason"] = ("XEX patch" if flags & XEX_MODULE_PATCH else "not a DLL") + \
            " (XexLoadImage loads DLL modules): not compiled"
        return entry
    entry["dll"] = True
    key = module_key(disc_path, used_keys)
    module_dir = Path(out, "modules", key)
    (module_dir / "ppc").mkdir(parents=True)
    entry.update({"key": key, "dir": f"modules/{key}", "symbol_prefix": f"rcomp_mod_{key}_", "_dir": module_dir})
    rc, _ = run([tools["decode"], str(path), str(module_dir / "plain.xex")], str(module_dir / "decode.json"),
                stderr_log=str(module_dir / "decode.log"), timeout=timeout)
    try:
        decode = json.loads((module_dir / "decode.json").read_text(encoding="utf-8")) if rc == 0 else None
    except ValueError:
        decode = None
    if not isinstance(decode, dict):
        entry["blocking_reasons"].append("decode failed: " + (module_dir / "decode.log").read_text(errors="replace")[:300])
        return entry
    entry["decode"] = {"encryption": decode.get("encryption"), "compression": decode.get("compression"),
                       "supported": decode.get("compression") in (0, 1, 2)}
    if not entry["decode"]["supported"]:
        entry["blocking_reasons"].append("unsupported compression/LZX form")
    try:
        x = (module_dir / "plain.xex").read_bytes()
        if x[:4] != b"XEX2":
            raise ValueError("decoded module is not an XEX2 file")
        info, image, records = parse_plain_xex(x)
        original = xex_original_name(x)
        exports, export_source = parse_exports(x, info, image)
    except (ValueError, struct.error, IndexError) as error:
        entry["blocking_reasons"].append("invalid decoded XEX: " + str(error))
        return entry
    code = [s for s in info["sections"] if s["code"]]
    entry.update({
        "original_name": original,
        # The name other modules import it by: the linker's output name, else the file name.
        "module_name": original or path.name,
        "image": {"base": f"0x{info['base']:08X}", "size": info["image_size"], "entry": f"0x{info['entry']:08X}",
                  "optional_headers": info["optional_headers"], "static_tls": info["static_tls"]},
        "sections": [{"name": s["name"], "size": s["size"], "code": s["code"]} for s in info["sections"]],
        "exports": {"source": export_source, "count": len(exports),
                    "code": sum(1 for v in exports.values()
                                if any(s["va"] <= v < s["va"] + s["size"] for s in code))},
        "_info": info, "_image": image, "_records": records, "_exports": exports, "_code": code,
    })
    return entry


def generate_module(m, tools, xenon_source, options, timeout, recompile):
    """XenonAnalyse and XenonRecomp for one AOT module, with the main module's generator options
    (the PPCContext layout is shared), symbol_prefix and module_import_libraries (patch 0022)."""
    out, info, image = m["_dir"], m["_info"], m["_image"]
    helpers = find_helpers(image, info["base"], info["sections"])
    m["register_helpers"] = {k: {"matches": len(v), "unique": len(v) == 1} for k, v in helpers.items()}
    rc_an, t_an = run([tools["analyse"], "plain.xex", "switch_tables.toml"], str(out / "xenonanalyse.log"),
                      cwd=str(out), timeout=timeout)
    tables = (out / "switch_tables.toml").read_text().count("[[switch]]") if (out / "switch_tables.toml").exists() else None
    unresolved = (out / "xenonanalyse.log").read_text(errors="replace").count("WARNING: unresolved jump table")
    m["jump_tables"] = {"xenonanalyse_exit": rc_an, "seconds": t_an, "tables": tables, "unresolved": unresolved}
    if rc_an != 0 or tables is None:
        m["blocking_reasons"].append("XenonAnalyse/jump-table analysis failed")
    if unresolved:
        m["blocking_reasons"].append(f"XenonAnalyse could not bound {unresolved} jump table(s) (see xenonanalyse.log)")
    libraries = list(m["_module_imports"])
    with open(out / "game.toml", "w") as f:
        f.write("[main]\nfile_path = \"plain.xex\"\nout_directory_path = \"ppc\"\n"
                "switch_table_file_path = \"switch_tables.toml\"\n")
        for k, hits in helpers.items():
            if len(hits) == 1:
                f.write(f"{k} = 0x{hits[0]:08X}\n")
        for key, value in sorted(options.items()):
            f.write(f"{key} = {value}\n")
        f.write(f"symbol_prefix = {json.dumps(m['symbol_prefix'])}\n")
        if libraries:
            f.write(f"module_import_libraries = {toml_string_list(libraries)}\n")
    m["xenonrecomp"] = {"ran": False}
    if not recompile:
        m["blocking_reasons"].append("recompilation was not run; inventory only")
        return
    rc, seconds = run([tools["recomp"], "game.toml", os.path.join(xenon_source, "XenonUtils", "ppc_context.h")],
                      str(out / "xenonrecomp.log"), cwd=str(out), timeout=timeout)
    log = (out / "xenonrecomp.log").read_text(errors="replace")
    absent = {"__" + name.removesuffix("_address") for name, hits in helpers.items() if not hits}
    counts, unimpl = classify_log(log, absent)
    mapping = out / "ppc" / "ppc_func_mapping.cpp"
    entries = read_mapping(mapping) if mapping.exists() else None
    generated = sum(p.stat().st_size for p in (out / "ppc").iterdir() if p.is_file())
    m["xenonrecomp"] = {"ran": True, "exit_code": rc, "seconds": seconds,
                        "functions": len(entries) if entries is not None else None,
                        "generated_bytes": generated, "warnings": counts, "unrecognized_instructions": unimpl,
                        "absent_helper_notices": sorted(n for n in absent if f"ERROR: {n} address is unspecified" in log),
                        "options": options, "symbol_prefix": m["symbol_prefix"], "module_import_libraries": libraries}
    if rc != 0:
        m["blocking_reasons"].append(f"XenonRecomp exited {rc}")
    if counts:
        m["blocking_reasons"].append("unsupported XenonRecomp warnings: " + ", ".join(counts))
    if unimpl:
        m["blocking_reasons"].append("unimplemented instructions: " + ", ".join(unimpl))
    if "unimplemented" in log.lower():
        m["blocking_reasons"].append("XenonRecomp emitted an unimplemented warning")
    if rc == 0 and entries:
        m["_mapping"] = entries
        m["functions"] = len(entries)
    else:
        m["blocking_reasons"].append("recompiler produced no usable functions or sources")


def check_module_table(m):
    """The generated table of a module: entry point present, addresses unique and inside the
    image, every symbol either unique by address or carrying the module's prefix."""
    entries = m.get("_mapping")
    if not entries:
        return
    info, prefix = m["_info"], m["symbol_prefix"]
    addresses = [address for address, _ in entries]
    if len(addresses) != len(set(addresses)):
        m["blocking_reasons"].append("duplicate guest addresses in generated function table")
    outside = [a for a in addresses if not info["base"] <= a < info["base"] + info["image_size"]]
    if outside:
        m["blocking_reasons"].append(f"{len(outside)} generated function(s) outside the module image")
    if info["entry"] and info["entry"] not in set(addresses):
        m["blocking_reasons"].append("entry point is absent from the generated function table")
    shared = re.compile(r"^(sub_[0-9A-F]+|__imp__(?!rcomp_)\w+)$")
    unprefixed = sorted(name for _, name in entries if not shared.match(name) and not name.startswith(prefix))
    if unprefixed:
        m["blocking_reasons"].append("generated symbols without the module prefix: " + ", ".join(unprefixed[:8]))
    # Exports a title can reach through XexGetProcedureAddress: those inside code must be functions.
    functions = set(addresses)
    loose = sorted(o for o, v in m["_exports"].items()
                   if any(s["va"] <= v < s["va"] + s["size"] for s in m["_code"]) and v not in functions)
    m["exports"]["code_without_function"] = [f"0x{o:04X}" for o in loose]
    if loose:
        m["blocking_reasons"].append("exported code addresses that are no recompiled function: ordinals "
                                     + ", ".join(m["exports"]["code_without_function"][:16]))


def find_helpers(image, base, sections):
    """Candidate addresses of the register save/restore helpers (unique matches only)."""
    sigs = {
        "savegprlr_14_address": [0xF9C1FF68, 0xF9E1FF70],  # std r14,-0x98(r1); std r15,-0x90(r1)
        "restgprlr_14_address": [0xE9C1FF68, 0xE9E1FF70],  # ld  r14,-0x98(r1); ld  r15,-0x90(r1)
        # The FPR helpers address the save area through r12, not r1 (XenonRecomp
        # README: `stfd f14, -0x90(r12)`): stfd/lfd fN,-(0x90 - 8*(N-14))(r12) for
        # f14..f31, then blr. The whole sequence keeps the match unique.
        "savefpr_14_address": [(54 << 26) | (n << 21) | (12 << 16) | ((8 * (n - 14) - 0x90) & 0xFFFF)
                               for n in range(14, 32)] + [0x4E800020],
        "restfpr_14_address": [(50 << 26) | (n << 21) | (12 << 16) | ((8 * (n - 14) - 0x90) & 0xFFFF)
                               for n in range(14, 32)] + [0x4E800020],
        "savevmx_14_address": [0x3960FEE0, 0x7DCB61CE],    # li r11,-0x120; stvx v14,r11,r12
        "restvmx_14_address": [0x3960FEE0, 0x7DCB60CE],    # li r11,-0x120; lvx  v14,r11,r12
    }
    words = {}
    for s in sections:
        if s["code"]:
            o = s["va"] - base
            words[s["va"]] = struct.unpack_from(f">{s['size'] // 4}I", image, o)
    found = {}
    for key, sig in sigs.items():
        hits = [va + 4 * i for va, w in words.items() for i in range(len(w) - len(sig) + 1)
                if all(w[i + j] == sig[j] for j in range(len(sig)))]
        found[key] = hits
    # VMX128 v64 variants: li r11,-0x400 then stvx128/lvx128 (Xenia: VX128_1(4, 451) / (4, 195)).
    for key, xo in (("savevmx_64_address", 0x1C3), ("restvmx_64_address", 0x0C3)):
        found[key] = [va + 4 * i for va, w in words.items() for i in range(len(w) - 1)
                      if w[i] == 0x3960FC00 and (w[i + 1] >> 26) == 4 and (w[i + 1] & 0x7F3) == xo]
    return found


def classify_log(text, absent_helpers=()):
    c = collections.Counter()
    unimpl = collections.Counter()
    for line in text.splitlines():
        m = re.match(r"Unrecognized instruction at 0x[0-9A-F]+: (\S+)", line)
        if m:
            unimpl[m.group(1)] += 1
        elif line.startswith("Unable to decode instruction"):
            c["undecodable_instructions"] += 1
        elif "with no switch table entry present" in line:
            c["switch_without_table"] += 1
        elif line.startswith("Function at") and "ends prematurely" in line:
            c["functions_ending_prematurely"] += 1
        elif line.startswith("Direct call to"):
            c["direct_calls_without_function"] += 1
        elif "Switch case at" in line:
            c["switch_case_outside_function"] += 1
        elif "has RC bit enabled but no comparison" in line:
            c["rc_bit_without_comparison"] += 1
        elif re.fullmatch(r"ERROR: (__(?:save|rest)(?:gprlr_14|fpr_14|vmx_14|vmx_64)) address is unspecified", line):
            helper = line.split()[1]
            if helper not in absent_helpers:
                c["helper_address_unspecified"] += 1
        elif line.lstrip().startswith(("ERROR:", "WARNING:", "WARN:")):
            c["unclassified_diagnostic"] += 1
    return dict(c), dict(unimpl.most_common())


def run(cmd, log, cwd=None, timeout=None, stderr_log=None):
    t = time.time()
    with open(log, "w") as f:
        try:
            if stderr_log is None:
                p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=cwd, timeout=timeout)
            else:
                with open(stderr_log, "w") as diagnostics:
                    p = subprocess.run(cmd, stdout=f, stderr=diagnostics, cwd=cwd, timeout=timeout)
            code = p.returncode
        except subprocess.TimeoutExpired:
            # subprocess.run kills and waits for this child. Keep partial logs
            # and return a failure so the inventory can still explain the gate.
            with open(stderr_log, "a") if stderr_log else contextlib.nullcontext(f) as diagnostics:
                diagnostics.write(f"\nERROR: tool timed out after {timeout} seconds\n")
            code = 124
    return code, round(time.time() - t, 1)


def prepare_output(path):
    if not output_allowed(path):
        raise ValueError("output is unsafe or inside a git work tree outside build/")
    out = Path(path).absolute()
    if out.exists():
        raise ValueError("output already exists; use a fresh run directory")
    out.mkdir(parents=True)
    (out / "ppc").mkdir()
    return str(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xex")
    ap.add_argument("--out", required=True)
    ap.add_argument("--recompile", action="store_true", help="also run XenonRecomp (long, several GB)")
    ap.add_argument("--tool-timeout", type=int, default=300,
                    help="maximum seconds per decoder/analyser/recompiler invocation (default: 300)")
    ap.add_argument("--require-supported", action="store_true",
                    help="fail unless the inventory is safe to compile and link (implies --recompile)")
    ap.add_argument("--decode-tool", default=TOOLS["decode"])
    ap.add_argument("--analyse-tool", default=TOOLS["analyse"])
    ap.add_argument("--recomp-tool", default=TOOLS["recomp"])
    ap.add_argument("--xenon-source", default=XENON_SRC, help="pinned, patched generator source tree")
    ap.add_argument("--function-hints", type=Path, metavar="JSON",
                    help="functions to add to the main module's generation: a JSON list of {address, size} "
                         "(tools/m6_code_pointer_functions.py writes it from a previous inventory)")
    ap.add_argument("--recomp-option", action="append", default=[], metavar="KEY=BOOL",
                    help="extra boolean key of the [main] table of game.toml (repeatable): " + ", ".join(RECOMP_OPTIONS))
    ap.add_argument("--modules", choices=("auto", "none"), default="auto",
                    help="auto (default): also inventory and recompile every other XEX2 DLL found under the disc "
                         "root into modules/<key>/ (AOT modules, rcomp/aot_modules.h); none: the main XEX only")
    ap.add_argument("--disc-root", help="game root holding the XEX and its modules (default: the XEX's folder)")
    a = ap.parse_args()
    if a.tool_timeout <= 0:
        ap.error("--tool-timeout must be positive")
    options = {}
    for item in a.recomp_option:
        key, _, value = item.partition("=")
        if key not in RECOMP_OPTIONS or value not in ("true", "false"):
            ap.error(f"--recomp-option {item!r}: expected KEY=true|false with KEY one of {', '.join(RECOMP_OPTIONS)}")
        options[key] = value
    function_hints = []
    if a.function_hints:
        try:
            raw = json.loads(a.function_hints.read_text(encoding="utf-8"))
            function_hints = [{"address": int(str(h["address"]), 0), "size": int(h["size"])} for h in raw]
        except (OSError, ValueError, KeyError, TypeError) as error:
            ap.error(f"--function-hints {a.function_hints}: {error}")
        if any(h["address"] % 4 or h["size"] <= 0 or h["size"] % 4 for h in function_hints):
            ap.error("--function-hints: every entry must be a 4-byte aligned, non-empty, 4-byte multiple range")
    selected_tools = {key: str(Path(path).resolve()) for key, path in
                      (("decode", a.decode_tool), ("analyse", a.analyse_tool), ("recomp", a.recomp_tool))}
    a.xenon_source = str(Path(a.xenon_source).resolve())
    if a.require_supported:
        a.recompile = True
    if not output_allowed(a.out):
        sys.exit(f"refusing {a.out}: inside a git work tree outside build/ (derived game content)")
    for k, p in selected_tools.items():
        if k == "recomp" and not a.recompile:
            continue
        if not os.path.exists(p):
            sys.exit(f"missing {p}: run tools/m6_setup.sh first")
    try:
        out = prepare_output(a.out)
    except (OSError, ValueError) as error:
        sys.exit(str(error))
    plain = os.path.join(out, "plain.xex")
    rc, _ = run([selected_tools["decode"], a.xex, plain], os.path.join(out, "decode.json"),
                stderr_log=os.path.join(out, "decode.log"), timeout=a.tool_timeout)
    if rc:
        sys.exit("decode failed: " + Path(out, "decode.log").read_text(errors="replace"))
    decode_text = Path(out, "decode.json").read_text(encoding="utf-8")
    try:
        decode = json.loads(decode_text)
        if not isinstance(decode, dict):
            raise ValueError("decoder report must be an object")
    except ValueError:
        sys.exit("decode did not report its encryption/compression form")
    try:
        info, image, records = parse_plain_xex(Path(plain).read_bytes())
    except (ValueError, struct.error, IndexError) as error:
        sys.exit("invalid decoded XEX: " + str(error))

    # AOT modules (rcomp/aot_modules.h): the other XEX2 DLLs of the disc, each recompiled into
    # modules/<key>/ with its symbols prefixed. Imports between modules are satisfied by the
    # providing module's exports; every other import joins the title's import report below.
    disc_root = Path(a.disc_root or os.path.dirname(os.path.abspath(a.xex))).resolve()
    candidates, skipped_dirs = find_disc_modules(disc_root, a.xex, out) if a.modules == "auto" else ([], [])
    aot, other_xex, used_keys = [], [], set()
    for path in candidates:
        entry = prepare_module(path, disc_root, out, used_keys, selected_tools, a.tool_timeout)
        (aot if entry.get("dll") else other_xex).append(entry)
    providers, provider_conflicts = {}, set()
    for m in aot:
        if "_info" not in m:
            continue
        for name in {m["module_name"].lower(), m["disc_path"].rsplit("/", 1)[-1].lower()}:
            if name in providers and providers[name] is not m:
                provider_conflicts.add(name)
            providers[name] = m
    main_module_imports = module_import_records(records, providers)
    title_records = [r for r in records if r["module"].lower() not in providers]
    for m in aot:
        if "_info" in m:
            m["_module_imports"] = module_import_records(m["_records"], providers)
            title_records += [r for r in m["_records"] if r["module"].lower() not in providers]

    tables = export_tables()
    done, done_variables, registration_units = source_imports(ROOT)
    source_inputs = ["runtime/CMakeLists.txt", "app/src/title_runtime.cpp", *registration_units]
    source_selection = {
        "scope": "static build and registration selection; not runtime execution proof",
        "files": {name: hashlib.sha256(Path(ROOT, name).read_bytes()).hexdigest()
                  for name in sorted(set(source_inputs))},
    }
    modules = collections.OrderedDict()
    # Without AOT modules title_records are exactly the main XEX's records.
    for r in title_records:
        mod = modules.setdefault(r["module"], {"functions": {}, "variables": {}, "unknown": set()})
        # A real XEX has a slot (type 0) and a thunk (type 1) per function
        # import; both carry the ordinal, names deduplicate them.
        t = tables.get(r["module"].lower(), {})
        if r["ordinal"] not in t:
            mod["unknown"].add(r["ordinal"])
            continue
        name, kind = t[r["ordinal"]]
        implemented = ((r["module"].lower(), r["ordinal"]) in
                       (done if kind == "function" else done_variables))
        (mod["functions"] if kind == "function" else mod["variables"])[name] = implemented
    imports_report = {}
    for m, d in modules.items():
        imports_report[m] = {
            "functions_total": len(d["functions"]),
            "functions_implemented": sorted(n for n, ok in d["functions"].items() if ok),
            "functions_missing": sorted(n for n, ok in d["functions"].items() if not ok),
            "variables_implemented": sorted(n for n, ok in d["variables"].items() if ok),
            "variables_missing": sorted(n for n, ok in d["variables"].items() if not ok),
            "unknown_ordinals": [f"0x{o:04X}" for o in sorted(d["unknown"])],
        }

    helpers = find_helpers(image, info["base"], info["sections"])
    switch_toml = os.path.join(out, "switch_tables.toml")
    rc_an, t_an = run([selected_tools["analyse"], plain, switch_toml], os.path.join(out, "xenonanalyse.log"),
                    timeout=a.tool_timeout)
    n_switch = Path(switch_toml).read_text().count("[[switch]]") if os.path.exists(switch_toml) else None
    # Generator patch 0021: a jump table XenonAnalyse recognises but cannot bound
    # is reported, not written; its bctr would stay an indirect call that leaves
    # the function, so it blocks like a generator diagnostic.
    unresolved_tables = Path(out, "xenonanalyse.log").read_text(errors="replace").count(
        "WARNING: unresolved jump table")

    toml = os.path.join(out, "game.toml")
    with open(toml, "w") as f:
        f.write("[main]\nfile_path = \"plain.xex\"\nout_directory_path = \"ppc\"\n"
                "switch_table_file_path = \"switch_tables.toml\"\n")
        for k, hits in helpers.items():
            if len(hits) == 1:
                f.write(f"{k} = 0x{hits[0]:08X}\n")
        for key, value in sorted(options.items()):
            f.write(f"{key} = {value}\n")
        if main_module_imports:  # generator patch 0022; absent when the XEX imports no AOT module
            f.write(f"module_import_libraries = {toml_string_list(main_module_imports)}\n")
        if function_hints:
            # Functions the generator's discovery misses (reached only through code pointers stored in
            # data: virtual method tables, callbacks; tools/m6_code_pointer_functions.py lists them):
            # XenonRecomp's `functions` array of the [main] table, recompiled like discovered ones.
            f.write("functions = [\n")
            for hint in function_hints:
                f.write(f"    {{ address = 0x{hint['address']:08X}, size = 0x{hint['size']:X} }},\n")
            f.write("]\n")
    recomp = {"ran": False}
    if a.recompile:
        rc_rc, t_rc = run([selected_tools["recomp"], "game.toml", os.path.join(a.xenon_source, "XenonUtils", "ppc_context.h")],
                          os.path.join(out, "xenonrecomp.log"), cwd=out, timeout=a.tool_timeout)
        generator_log = Path(out, "xenonrecomp.log").read_text(errors="replace")
        # Upstream prints these eight configuration notices unconditionally
        # for zero addresses, even when a title has no such helper signature.
        # Ambiguous/present helpers and all other ERROR diagnostics block.
        absent_helpers = {"__" + name.removesuffix("_address") for name, hits in helpers.items() if not hits}
        counts, unimpl = classify_log(generator_log, absent_helpers)
        mapping = os.path.join(out, "ppc", "ppc_func_mapping.cpp")
        nfunc = sum(1 for l in Path(mapping).read_text().splitlines() if l.strip().startswith("{ 0x")) if os.path.exists(mapping) else None
        gen_bytes = sum(os.path.getsize(os.path.join(out, "ppc", f)) for f in os.listdir(os.path.join(out, "ppc")))
        recomp = {"ran": True, "exit_code": rc_rc, "seconds": t_rc, "functions": nfunc,
                  "generated_bytes": gen_bytes, "warnings": counts, "unrecognized_instructions": unimpl,
                  "absent_helper_notices": sorted(name for name in absent_helpers
                      if f"ERROR: {name} address is unspecified" in generator_log)}
        recomp["options"] = options
        recomp["function_hints"] = len(function_hints)

    # AOT modules: generation with the main module's options, then the checks that need every
    # module's function table (imports between modules, entry points, prefixes, image ranges).
    functions_by_module = {}
    for m in aot:
        if "_info" in m:
            generate_module(m, selected_tools, a.xenon_source, options, a.tool_timeout, a.recompile)
            if m.get("_mapping") is not None:
                functions_by_module[m["disc_path"]] = {address for address, _ in m["_mapping"]}
    module_imports_report, main_module_blockers = verify_module_imports(main_module_imports, functions_by_module)
    ranges = [("default XEX", info["base"], info["image_size"])]
    for m in aot:
        if "_info" not in m:
            continue
        m["module_imports"], blockers = verify_module_imports(m.pop("_module_imports"), functions_by_module)
        m["blocking_reasons"] += blockers
        check_module_table(m)
        ranges.append((m["disc_path"], m["_info"]["base"], m["_info"]["image_size"]))
    overlaps = [f"image ranges overlap: {x[0]} and {y[0]}" for i, x in enumerate(ranges) for y in ranges[i + 1:]
                if x[1] < y[1] + y[2] and y[1] < x[1] + x[2]]
    compiled = [m for m in aot if m.get("_mapping")]
    if compiled:
        for m in compiled:
            write_module_descriptor(m["_dir"], m["key"], m["disc_path"], m["_mapping"])
            m["descriptor"] = f"{m['dir']}/aot_module.cpp"
            try:
                m["artifacts"] = artifact_snapshot(m["_dir"])
                with open(Path(m["_dir"], "aot_module.cpp"), "rb") as data:
                    m["artifacts"]["aot_module.cpp"] = {"bytes": Path(m["_dir"], "aot_module.cpp").stat().st_size,
                                                        "sha256": hashlib.file_digest(data, "sha256").hexdigest()}
            except (OSError, ValueError) as error:
                m["blocking_reasons"].append("generated artifact validation failed: " + str(error))
        write_module_list(out, compiled)
    modules_report = {
        "mode": a.modules, "disc_root_files_scanned": a.modules == "auto", "skipped_folders": skipped_dirs,
        "aot": [{k: v for k, v in m.items() if not k.startswith("_")} for m in aot],
        "other_xex": other_xex,
        "list": "aot_modules.cpp" if compiled else None,
    }

    report = {
        "tool": "tools/m6_inventory.py", "r_comp_commit": subprocess.run(
            # The sandbox/Cygwin user can differ from the checkout owner. Trust
            # this explicitly selected workspace for this read only; never
            # change the user's global Git ownership configuration.
            ["git", "-c", "safe.directory=" + ROOT, "-C", ROOT,
             "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip(),
        "decode": {"encryption": decode.get("encryption"), "compression": decode.get("compression"),
                   "supported": decode.get("compression") in (0, 1, 2)},
        "source_xex": {"size": os.path.getsize(a.xex),
                       "sha256": hashlib.sha256(Path(a.xex).read_bytes()).hexdigest()},
        "image": {"base": f"0x{info['base']:08X}", "size": info["image_size"], "entry": f"0x{info['entry']:08X}",
                  "optional_headers": info["optional_headers"], "static_tls": info["static_tls"],
                  "execution_info": info["execution_info"]},
        "sections": [{"name": s["name"], "size": s["size"], "code": s["code"]} for s in info["sections"]],
        "imports": imports_report,
        "runtime_source_selection": source_selection,
        "register_helpers": {k: {"matches": len(v), "unique": len(v) == 1} for k, v in helpers.items()},
        "setjmp_longjmp": "not detected automatically (identify by hand if XenonRecomp reports setjmp use)",
        "jump_tables": {"xenonanalyse_exit": rc_an, "seconds": t_an, "tables": n_switch,
                        "unresolved": unresolved_tables},
        "xenonrecomp": recomp,
        # Title scope: "imports" above covers default.xex and its AOT modules (one runtime serves
        # them all); imports between modules are listed here and per module.
        "module_imports": module_imports_report,
        "modules": modules_report,
    }

    L = ["# M6 inventory", "", f"R-comp {report['r_comp_commit']} — metadata only (no game bytes).", "",
         f"Image: size {info['image_size']} bytes, {len(info['sections'])} sections "
         f"({', '.join(s['name'] + ':' + str(s['size']) for s in info['sections'])}).", "",
         "XEX identity (raw version words): " + json.dumps(info["execution_info"], sort_keys=True), "",
         "SHA-256 of the original XEX: `" + report["source_xex"]["sha256"] + "`.", "",
         "## Imports", "", "| Module | Functions | Implemented | Missing | Variables | Unknown ordinals |",
         "| --- | --- | --- | --- | --- | --- |"]
    for m, r in imports_report.items():
        L.append(f"| {m} | {r['functions_total']} | {len(r['functions_implemented'])} | {len(r['functions_missing'])} "
                 f"| {len(r['variables_implemented']) + len(r['variables_missing'])} | {len(r['unknown_ordinals'])} |")
    for m, r in imports_report.items():
        L += ["", f"### {m}: missing functions", "", ", ".join(r["functions_missing"]) or "(none)"]
        if r["variables_implemented"] or r["variables_missing"]:
            L += ["", "Implemented variables: " + (", ".join(r["variables_implemented"]) or "(none)"),
                  "Missing variables: " + (", ".join(r["variables_missing"]) or "(none)")]
    L += ["", "## Register save/restore routines", ""]
    L += [f"- {k}: {v['matches']} match(es){' (selected)' if v['unique'] else ''}"
          for k, v in report["register_helpers"].items()]
    L += ["", f"## Jump tables: {n_switch} (XenonAnalyse, exit code {rc_an}, {t_an} s)", ""]
    if recomp["ran"]:
        L += [f"## XenonRecomp: exit code {recomp['exit_code']}, {recomp['seconds']} s, {recomp['functions']} functions, "
              f"{recomp['generated_bytes']} bytes generated", ""]
        L += [f"- {k}: {v}" for k, v in recomp["warnings"].items()] or ["- no classified warning"]
        if recomp.get("options"):
            L += ["", "Generator options: " + ", ".join(f"{k}={v}" for k, v in sorted(recomp["options"].items()))]
        L += ["", "Unrecognized instructions: " + (", ".join(f"{k} ×{v}" for k, v in
                                                         recomp["unrecognized_instructions"].items()) or "none")]
    else:
        L += ["XenonRecomp not run (option --recompile)."]
    if aot or other_xex:
        L += ["", "## AOT modules (other XEX2 files of the disc)", "",
              "| Disc path | Module name | Base | Size | Entry | Functions | Generator | Blockers |",
              "| --- | --- | --- | --- | --- | --- | --- | --- |"]
        for m in aot:
            image_info = m.get("image", {})
            generator = m.get("xenonrecomp", {})
            L.append(f"| {m['disc_path']} | {m.get('module_name', '?')} | {image_info.get('base', '?')} | "
                     f"{image_info.get('size', '?')} | {image_info.get('entry', '?')} | {m.get('functions', '-')} | "
                     f"exit {generator.get('exit_code', '-')}, warnings {len(generator.get('warnings') or {})} | "
                     f"{len(m['blocking_reasons'])} |")
        for m in other_xex:
            L.append(f"| {m['disc_path']} | - | - | - | - | - | {m['reason']} | 0 |")
        for m in aot:
            if m["blocking_reasons"]:
                L += ["", f"### {m['disc_path']}: blockers", ""] + [f"- {r}" for r in m["blocking_reasons"]]
    Path(out, "INVENTORY.md").write_text("\n".join(L) + "\n", encoding="utf-8")
    failures = []
    if not report["decode"]["supported"]:
        failures.append("unsupported compression/LZX form")
    if rc_an != 0 or n_switch is None:
        failures.append("XenonAnalyse/jump-table analysis failed")
    if unresolved_tables:
        failures.append(f"XenonAnalyse could not bound {unresolved_tables} jump table(s) (see xenonanalyse.log)")
    for module, imported in imports_report.items():
        if imported["functions_missing"]:
            failures.append(f"unsupported imports in {module}: " + ", ".join(imported["functions_missing"]))
        if imported["variables_missing"]:
            failures.append(f"unsupported kernel variables in {module}: " + ", ".join(imported["variables_missing"]))
        if imported["unknown_ordinals"]:
            failures.append(f"unknown imports in {module}: " + ", ".join(imported["unknown_ordinals"]))
    if a.recompile:
        if recomp["exit_code"] != 0:
            failures.append(f"XenonRecomp exited {recomp['exit_code']}")
        if recomp["warnings"]:
            failures.append("unsupported XenonRecomp warnings: " + ", ".join(recomp["warnings"]))
        if recomp["unrecognized_instructions"]:
            failures.append("unimplemented instructions: " + ", ".join(recomp["unrecognized_instructions"]))
        log_lower = Path(out, "xenonrecomp.log").read_text(errors="replace").lower()
        if "unimplemented" in log_lower:
            failures.append("XenonRecomp emitted an unimplemented warning")
    if not recomp["ran"]:
        failures.append("recompilation was not run; inventory only")
    elif not recomp.get("functions") or not recomp.get("generated_bytes"):
        failures.append("recompiler produced no usable functions or sources")
    if recomp["ran"]:
        try:
            report["artifacts"] = artifact_snapshot(out)
        except (OSError, ValueError) as error:
            failures.append("generated artifact validation failed: " + str(error))
    failures.extend(static_tls_blockers(report["image"]))
    failures += [f"several AOT modules answer to the import library name {name}" for name in sorted(provider_conflicts)]
    failures += ["default XEX: " + reason for reason in main_module_blockers]
    failures += overlaps
    for m in aot:
        failures += [f"module {m['disc_path']}: {reason}" for reason in m["blocking_reasons"]]
        if m.get("image", {}).get("static_tls"):
            failures.append(f"module {m['disc_path']}: static TLS in a DLL module: rcomp/aot_modules.h carries no TLS description")
    report["supported_for_link"] = not failures
    report["blocking_reasons"] = failures
    Path(out, "inventory.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {out}/INVENTORY.md and inventory.json (shareable); plain.xex, ppc/, logs and tomls stay local")
    if a.require_supported and failures:
        for reason in failures:
            print("FAIL: " + reason, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
