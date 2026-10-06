#!/usr/bin/env python3
"""Build a synthetic, unencrypted, uncompressed XEX2 from original PPC assembly.

Purpose: exercise XenonRecomp's XEX/TOML mode (the mode a real game uses:
PE sections, .pdata function discovery, import thunks, entry point, function
mapping table) and our runtime's image loader, with no game content.

Annotations in the .s file:
  #_ XEX_ENTRY <label>                         entry point
  #_ XEX_IMPORT <label> <module> <ordinal>     16-byte function import thunk (record type 1)
  #_ XEX_IMPORT_RECORD <label> <module> <ordinal>  4-byte import slot in .data (record type 0),
                                               filled by the runtime loader
  #_ XEX_LIBRARY_VERSION <module> <version> <minimum_version>  raw version words
  #_ XEX_TLS <label> <raw_size> <data_size> <slot_count>  original static TLS template
  #_ XEX_HEADER_VALUE <key> <u32>             optional header ending in 00 or 01
  #_ XEX_HEADER_DATA <key> <label> <size>      copy linked bytes into a header block:
                                             low byte 02..FE => size == low byte * 4;
                                             FF => first BE32 is the total size
  #_ XEX_HEADER_VALUE 0x00030000 <flags>       system flags / original privilege fixture
  #_ XEX_FUNCTION_END <label> <end label>      the .pdata length of function <label> stops at
                                             <end label> (code or padding .pdata does not list);
                                             <end label> itself is not a function
Every other global label in .text is listed in .pdata as a function.

Layout (image base 0x82000000, or --base for fixtures linked as several modules):
  +0x0000  PE headers (DOS + NT + section table), as XenonUtils/xex.cpp reads them
  +0x1000  .text   (IMAGE_SCN_CNT_CODE)
  next 4K  .rdata, .data, .pdata
XEX2: header, optional headers (file format NONE/NONE, entry point, image base,
import libraries), security info (image size, load address), then the image.

Output: <out>.xex, <out>.elf (linked), <out>.json (sections, symbols, imports).
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys

BASE = 0x82000000
PREFIX = "powerpc-linux-gnu-"
LINKER_SCRIPT = """
SECTIONS {
  . = 0x82001000;
  .text : { *(.text) }
  . = ALIGN(0x1000);
  .rdata : { *(.rodata) *(.rodata.*) }
  . = ALIGN(0x1000);
  .data : { *(.data) *(.sdata) *(.bss) *(.sbss) }
  /DISCARD/ : { *(.comment) *(.note*) *(.gnu*) }
}
"""


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0 or p.stderr.strip():
        raise SystemExit(f"{' '.join(cmd)} failed:\n{p.stdout}{p.stderr}")
    return p.stdout


def section_info(elf):
    out = {}
    for line in run([PREFIX + "readelf", "-SW", elf]).splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m and m.group(1) in (".text", ".rdata", ".data"):
            out[m.group(1)] = (int(m.group(2), 16), int(m.group(4), 16))
    return out


def section_bytes(elf, name, tmp):
    run([PREFIX + "objcopy", "-O", "binary", "-j", name, elf, tmp])
    with open(tmp, "rb") as f:
        return f.read()


def symbols(elf):
    syms = {}
    for line in run([PREFIX + "nm", "--numeric-sort", elf]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = (int(parts[0], 16), parts[1])
    return syms


def align(x, a):
    return (x + a - 1) & ~(a - 1)


def u32(text, context):
    try:
        value = int(text, 0)
    except ValueError:
        raise SystemExit(f"{context}: expected a 32-bit unsigned integer")
    if not 0 <= value <= 0xFFFFFFFF:
        raise SystemExit(f"{context}: integer outside 32-bit unsigned range")
    return value


def main():
    global BASE
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--out", required=True, help="output path without extension")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=BASE,
                    help="image base, 64 KiB aligned (default 0x82000000); several fixtures of one program "
                         "(AOT modules) need distinct bases")
    args = ap.parse_args()
    if args.base & 0xFFFF or not 0x10000 <= args.base <= 0xF0000000:
        raise SystemExit("--base must be a 64 KiB aligned address below 0xF0000000")
    BASE = args.base

    entry_label, imports, records, tls = None, [], [], None
    custom_headers = []
    custom_keys = set()
    library_versions = {}
    function_ends = {}
    for line_number, line in enumerate(open(args.source), 1):
        m = re.fullmatch(r"\s*#_\s+XEX_FUNCTION_END\s+(\S+)\s+(\S+)\s*", line)
        if m:
            if m.group(1) in function_ends:
                raise SystemExit(f"line {line_number}: duplicate XEX_FUNCTION_END for {m.group(1)}")
            function_ends[m.group(1)] = m.group(2)
        elif re.match(r"\s*#_\s+XEX_FUNCTION_END\b", line):
            raise SystemExit(f"line {line_number}: malformed XEX_FUNCTION_END annotation")
        m = re.match(r"\s*#_\s+XEX_ENTRY\s+(\S+)", line)
        if m:
            if entry_label is not None:
                raise SystemExit("duplicate #_ XEX_ENTRY")
            entry_label = m.group(1)
        m = re.match(r"\s*#_\s+XEX_IMPORT\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if m:
            imports.append((m.group(1), m.group(2), int(m.group(3), 0)))
        m = re.match(r"\s*#_\s+XEX_IMPORT_RECORD\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if m:
            records.append((m.group(1), m.group(2), int(m.group(3), 0)))
        m = re.fullmatch(r"\s*#_\s+XEX_LIBRARY_VERSION\s+(\S+)\s+(\S+)\s+(\S+)\s*", line)
        if m:
            module, version, minimum = m.groups()
            if module in library_versions:
                raise SystemExit(f"line {line_number}: duplicate XEX_LIBRARY_VERSION for {module}")
            library_versions[module] = (u32(version, 'XEX_LIBRARY_VERSION'),
                                        u32(minimum, 'XEX_LIBRARY_VERSION'))
            if library_versions[module][1] > library_versions[module][0]:
                raise SystemExit(f"line {line_number}: minimum library version exceeds version")
        elif re.match(r"\s*#_\s+XEX_LIBRARY_VERSION\b", line):
            raise SystemExit(f"line {line_number}: malformed XEX_LIBRARY_VERSION annotation")
        m = re.match(r"\s*#_\s+XEX_TLS\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s*$", line)
        if m:
            if tls is not None:
                raise SystemExit("duplicate #_ XEX_TLS")
            label, raw_size, data_size, slots = m.groups()
            tls = (label, int(raw_size, 0), int(data_size, 0), int(slots, 0))
        m = re.fullmatch(r"\s*#_\s+XEX_HEADER_(VALUE|DATA)\s+(.+?)\s*", line)
        if m:
            kind, fields = m.group(1), m.group(2).split()
            context = f"line {line_number} XEX_HEADER_{kind}"
            if len(fields) != (2 if kind == "VALUE" else 3):
                raise SystemExit(f"{context}: wrong number of arguments")
            key = u32(fields[0], context)
            if key in custom_keys:
                raise SystemExit(f"{context}: duplicate optional header key 0x{key:08X}")
            custom_keys.add(key)
            words = key & 0xFF
            if kind == "VALUE":
                if words not in (0, 1):
                    raise SystemExit(f"{context}: VALUE requires a key ending in 00 or 01")
                custom_headers.append((key, kind, u32(fields[1], context), None))
            else:
                size = u32(fields[2], context)
                if words < 2 or (words != 0xFF and size != words * 4) or (words == 0xFF and size < 4):
                    raise SystemExit(f"{context}: DATA size does not match the optional header key")
                custom_headers.append((key, kind, fields[1], size))
        elif re.match(r"\s*#_\s+XEX_HEADER_", line):
            raise SystemExit(f"line {line_number}: malformed XEX_HEADER annotation")
    if not entry_label:
        raise SystemExit("missing #_ XEX_ENTRY")

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    obj, elf, ld = out + ".o", out + ".elf", out + ".ld"
    with open(ld, "w") as f:
        f.write(LINKER_SCRIPT.replace("0x82001000", f"0x{BASE + 0x1000:08X}"))
    run([PREFIX + "as", "-a32", "-be", "-mregnames", "-mpower7", "-maltivec", "-o", obj, args.source])
    run([PREFIX + "ld", "-m", "elf32ppc", "--no-relax", "-T", ld, "-o", elf, obj])
    secs = section_info(elf)
    syms = symbols(elf)
    import_labels = {lab for lab, _, _ in imports}

    # --- image -----------------------------------------------------------
    blobs = {n: section_bytes(elf, n, out + n + ".bin") for n in secs}
    text_va, text_size = secs[".text"]
    if entry_label not in syms or syms[entry_label][1] not in "tT" or not (
            text_va <= syms[entry_label][0] < text_va + text_size) or syms[entry_label][0] & 3:
        raise SystemExit("XEX_ENTRY must name an aligned instruction in .text")
    if entry_label in import_labels:
        raise SystemExit("XEX_ENTRY cannot be an import thunk")
    funcs = sorted((a, n) for n, (a, t) in syms.items()
                   if t in "tT" and text_va <= a < text_va + text_size and n not in import_labels
                   and not n.startswith(".L") and n not in function_ends.values())
    pdata = b""
    unknown_ends = sorted(set(function_ends) - {n for _, n in funcs})
    if unknown_ends:
        raise SystemExit("XEX_FUNCTION_END names no .pdata function: " + ", ".join(unknown_ends))
    thunk_addrs = sorted(syms[l][0] for l in import_labels)
    for i, (a, n) in enumerate(funcs):
        end = text_va + text_size
        nexts = [x for x, _ in funcs[i + 1:]] + [t for t in thunk_addrs if t > a]
        if nexts:
            end = min(nexts)
        if n in function_ends:
            label = function_ends[n]
            if label not in syms or syms[label][0] & 3 or not a < syms[label][0] <= end:
                raise SystemExit(f"XEX_FUNCTION_END {n}: {label} must be an aligned address after {n}, "
                                 "at or before the next function")
            end = syms[label][0]
        length = (end - a) // 4
        pdata += struct.pack(">II", a, (length << 8) | (1 << 30))
    last_end = max(va + len(blobs[n]) for n, (va, _) in secs.items())
    pdata_va = align(last_end, 0x1000)
    sections = [(n, secs[n][0], blobs[n]) for n in (".text", ".rdata", ".data") if n in secs]
    sections.append((".pdata", pdata_va, pdata))
    image_size = align(pdata_va + len(pdata), 0x1000) - BASE
    image = bytearray(image_size)
    for n, va, data in sections:
        image[va - BASE:va - BASE + len(data)] = data

    # Import thunks: first word = type (1 = function) << 24 | ordinal.
    for lab, module, ordinal in imports:
        va = syms[lab][0]
        struct.pack_into(">IIII", image, va - BASE, (1 << 24) | ordinal, 0, 0, 0)
    for lab, module, ordinal in records:
        struct.pack_into(">I", image, syms[lab][0] - BASE, ordinal)

    # PE32 headers. The outer XEX is BE; all PE scalar fields remain LE.
    # This original image has identical raw offsets and RVAs, with section
    # payloads padded in the image to FileAlignment. See README-mkxex.md.
    e_lfanew = 0x80
    struct.pack_into("<H", image, 0, 0x5A4D)
    struct.pack_into("<I", image, 0x3C, e_lfanew)
    struct.pack_into("<I", image, e_lfanew, 0x00004550)                  # "PE\0\0"
    struct.pack_into("<HH", image, e_lfanew + 4, 0x01F2, len(sections))  # Machine, NumberOfSections
    struct.pack_into("<H", image, e_lfanew + 20, 224)                    # SizeOfOptionalHeader
    struct.pack_into("<H", image, e_lfanew + 22, 0x0103)                 # reloc stripped, executable, 32-bit
    pe_opt = e_lfanew + 24
    raw_sizes = {n: align(len(data), 0x200) for n, _, data in sections}
    data_rva = next((va - BASE for n, va, _ in sections if n != ".text"), 0)
    struct.pack_into("<HBBIIIIII", image, pe_opt, 0x10B, 0, 0,
                     raw_sizes[".text"], sum(v for n, v in raw_sizes.items() if n != ".text"),
                     0, syms[entry_label][0] - BASE, text_va - BASE, data_rva)
    struct.pack_into("<III", image, pe_opt + 28, BASE, 0x1000, 0x200)
    struct.pack_into("<II", image, pe_opt + 56, image_size, 0x1000)
    struct.pack_into("<H", image, pe_opt + 68, 14)                       # IMAGE_SUBSYSTEM_XBOX
    struct.pack_into("<IIIIII", image, pe_opt + 72, 0x100000, 0x1000,
                     0x100000, 0x1000, 0, 16)                          # stack/heap, loader flags, directories
    sec_table = e_lfanew + 4 + 20 + 224
    for k, (n, va, data) in enumerate(sections):
        o = sec_table + 40 * k
        image[o:o + 8] = n.encode().ljust(8, b"\0")
        flags = 0x60000020 if n == ".text" else (0xC0000040 if n == ".data" else 0x40000040)
        struct.pack_into("<IIIIIIHHI", image, o + 8, len(data), va - BASE, raw_sizes[n], va - BASE,
                         0, 0, 0, 0, flags)
    assert sec_table + 40 * len(sections) <= 0x1000

    # --- XEX2 container ---------------------------------------------------
    libs = []
    for lab, module, ordinal in imports + records:
        if module not in [m for m, _ in libs]:
            libs.append((module, []))
        dict(libs)[module].append(syms[lab][0])
    strtab = b""
    unknown_libraries = set(library_versions) - {module for module, _ in libs}
    if unknown_libraries:
        raise SystemExit('XEX_LIBRARY_VERSION has no imports: ' + ', '.join(sorted(unknown_libraries)))
    for m, _ in libs:
        s = m.encode() + b"\0"
        strtab += s + b"\0" * ((4 - len(s) % 4) % 4)
    lib_blobs = b""
    for idx, (m, thunks) in enumerate(libs):
        version, minimum = library_versions.get(m, (0x20000000, 0x20000000))
        body = struct.pack(">I", 0) + b"\0" * 0x14 + struct.pack(">IIIHH", 0, version, minimum, idx, len(thunks))
        body += b"".join(struct.pack(">I", t) for t in thunks)
        body = struct.pack(">I", len(body)) + body[4:]
        lib_blobs += body
    import_hdr = struct.pack(">III", 12 + len(strtab) + len(lib_blobs), len(strtab), len(libs)) + strtab + lib_blobs
    file_format = struct.pack(">IHH", 8, 0, 0)  # infoSize, encryption NONE, compression NONE

    opt = []  # (key, value_or_blob)
    opt.append((0x000003FF, file_format))
    opt.append((0x00010100, syms[entry_label][0]))
    opt.append((0x00010201, BASE))
    if imports or records:
        opt.append((0x000103FF, import_hdr))
    tls_meta = None
    if tls is not None:
        label, raw_size, data_size, slots = tls
        if label not in syms or not (0 <= raw_size <= data_size <= 0xFFFFFFFF) or not (0 <= slots <= 0xFFFFFFFF):
            raise SystemExit("invalid #_ XEX_TLS sizes or label")
        address = syms[label][0]
        if data_size + 4 * slots > 0xFFFFFFFF or not any(
                va <= address and address + raw_size <= va + len(data)
                for _, va, data in sections):
            raise SystemExit("XEX TLS template must fit an image section and allocation must fit 32 bits")
        # XEX_HEADER_TLS_INFO: slot_count, raw_data_address, data_size, raw_data_size.
        opt.append((0x00020104, struct.pack(">IIII", slots, address, data_size, raw_size)))
        tls_meta = dict(slot_count=slots, raw_data_address=address, data_size=data_size, raw_data_size=raw_size)
    automatic_keys = {key for key, _ in opt}
    for key, kind, value, size in custom_headers:
        if key in automatic_keys:
            raise SystemExit(f"custom optional header collides with automatic key 0x{key:08X}")
        if kind == "VALUE":
            opt.append((key, value))
            continue
        label = value
        if label not in syms:
            raise SystemExit(f"XEX_HEADER_DATA unknown label {label}")
        address = syms[label][0]
        if not any(va <= address and address + size <= va + len(data) for _, va, data in sections):
            raise SystemExit(f"XEX_HEADER_DATA {label}: payload must fit one image section")
        payload = bytes(image[address - BASE:address - BASE + size])
        if key & 0xFF == 0xFF and struct.unpack_from(">I", payload)[0] != size:
            raise SystemExit(f"XEX_HEADER_DATA {label}: FF payload must begin with its total BE32 size")
        opt.append((key, payload))
    header_len = 24 + 8 * len(opt)
    blob_area, blobs_out, cursor = [], {}, header_len
    for key, val in opt:
        if isinstance(val, bytes):
            blobs_out[key] = cursor
            blob_area.append(val)
            cursor += align(len(val), 8)
    security_off = align(cursor, 8)
    security = struct.pack(">II", 388, image_size) + b"\0" * 0x100 + struct.pack(">III", 0, 0, BASE)
    security = security.ljust(388, b"\0")
    header_size = align(security_off + len(security), 0x1000)
    xex = bytearray(header_size)
    struct.pack_into(">IIIIII", xex, 0, 0x58455832, 0, header_size, 0, security_off, len(opt))
    for k, (key, val) in enumerate(opt):
        v = blobs_out[key] if isinstance(val, bytes) else val
        struct.pack_into(">II", xex, 24 + 8 * k, key, v)
    for key, val in opt:
        if isinstance(val, bytes):
            xex[blobs_out[key]:blobs_out[key] + len(val)] = val
    xex[security_off:security_off + len(security)] = security
    xex += image
    with open(out + ".xex", "wb") as f:
        f.write(xex)

    meta = {
        "base": BASE, "image_size": image_size, "entry": syms[entry_label][0], "entry_label": entry_label,
        "sections": {n: {"va": va, "size": len(d)} for n, va, d in sections},
        "functions": {n: a for a, n in funcs},
        "imports": [{"label": l, "module": m, "ordinal": o, "thunk": syms[l][0]} for l, m, o in imports],
        "import_records": [{"label": l, "module": m, "ordinal": o, "slot": syms[l][0]} for l, m, o in records],
        "library_versions": {m: {'version': library_versions.get(m, (0x20000000, 0x20000000))[0],
                                 'minimum_version': library_versions.get(m, (0x20000000, 0x20000000))[1]}
                              for m, _ in libs},
        "tls": tls_meta,
        "pe": {"nt_headers_rva": e_lfanew, "optional_magic": 0x10B,
               "file_alignment": 0x200, "section_alignment": 0x1000, "headers_size": 0x1000},
        "optional_headers": [{"key": key, "kind": "data" if isinstance(val, bytes) else "value",
                              "value": blobs_out[key] if isinstance(val, bytes) else val,
                              "size": len(val) if isinstance(val, bytes) else 4}
                             for key, val in opt],
    }
    with open(out + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    print(json.dumps({"xex": out + ".xex", "bytes": len(xex), "functions": len(funcs), "imports": len(imports)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
