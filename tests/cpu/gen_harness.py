#!/usr/bin/env python3
"""Generate the CPU test harness tables from fixture annotations (Agent 6).

Oracle = the `#_ REGISTER_IN/OUT`, `MEMORY_IN/OUT`, `EXPECT_FATAL`
annotations of each .s fixture, parsed here independently of XenonRecomp.
Addresses come from the assembler's symbol table (nm), not from XenonRecomp.
The only facts taken from the generated C++ are *which* functions exist and
whether their bodies contain a generator trap, both used to refuse a test,
never to compute an expected value.

Output: <out>/cases_<set>_<stem>.cpp (one per generated fixture) and
<out>/cases_index.cpp, plus <out>/cases.json (the static classification:
tests that cannot run are reported with their status here, not skipped).
"""
import argparse
import json
import os
import re
import struct
import sys

ANN_RE = re.compile(r"^\s*#_\s+(\w+)\s*(.*)$")
LABEL_RE = re.compile(r"^([A-Za-z_][\w.$]*):")
FUNC_RE = re.compile(r"PPC_WEAK_FUNC\((\w+)\);\s*\nPPC_FUNC_IMPL\(__imp__\1\) \{\n(.*?)\n\}\n", re.S)
CALL_RE = re.compile(r"\b(\w+)\(ctx, base\);")


def parse_fixture(path, imports=None):
    """Returns list of tests: {name, ins, outs, mem_in, mem_out, expect_fatal, program}.

    `imports` (dict, optional) receives helper label -> (module, name) for
    `#_ IMPORT` annotations placed under a non-test label."""
    tests, cur, label = [], None, None
    for raw in open(path):
        line = raw.split(";")[0].rstrip("\n")
        m = LABEL_RE.match(line.strip())
        if m and not line.lstrip().startswith("#"):
            name = m.group(1)
            label = name
            cur = None
            if name.startswith("test_"):
                cur = {"name": name, "ins": [], "outs": [], "mem_in": [], "mem_out": [], "expect_fatal": None,
                       "program": None, "gfx_expect": None}
                tests.append(cur)
            continue
        a = ANN_RE.match(line)
        if a and a.group(1) == "HLE":
            if imports is None or label is None or label.startswith("test_"):
                raise SystemExit(f"{path}: HLE must follow a helper label")
            imports[label] = ("hle", a.group(2).strip())
            continue
        if a and a.group(1) == "IMPORT":
            module, name = a.group(2).split()
            if imports is None or label is None or label.startswith("test_"):
                raise SystemExit(f"{path}: IMPORT must follow a helper label")
            imports[label] = (module, name)
            continue
        if not a or cur is None:
            continue
        kind, rest = a.group(1), a.group(2).strip()
        if kind in ("REGISTER_IN", "REGISTER_OUT"):
            reg, val = rest.split(None, 1)
            (cur["ins"] if kind == "REGISTER_IN" else cur["outs"]).append(parse_reg(reg, val.strip()))
        elif kind in ("MEMORY_IN", "MEMORY_OUT"):
            addr, *bs = rest.split()
            data = bytes.fromhex("".join(bs))
            (cur["mem_in"] if kind == "MEMORY_IN" else cur["mem_out"]).append((int(addr, 16), data))
        elif kind == "EXPECT_FATAL":
            cur["expect_fatal"] = rest
        elif kind == "PROGRAM":
            cur["program"] = rest
        elif kind == "GFX_EXPECT":
            cur["gfx_expect"] = rest
        else:
            raise SystemExit(f"{path}: unknown annotation {kind}")
    return tests


def parse_int(v):
    v = v.lower().rstrip("ul")
    n = int(v, 0)
    return n & 0xFFFFFFFFFFFFFFFF


def parse_reg(reg, val):
    if reg.startswith("v") and reg[1:].isdigit():
        words = [int(w.strip(), 16) for w in val.strip("[]").split(",")]
        assert len(words) == 4, (reg, val)
        return {"reg": reg, "kind": "VR", "w": words}
    if reg.startswith("f") and reg[1:].isdigit():
        if val.lower().startswith("0x") or val.lower().startswith("-0x"):
            return {"reg": reg, "kind": "FBITS", "u": parse_int(val)}
        bits = struct.unpack("<Q", struct.pack("<d", float(val)))[0]
        return {"reg": reg, "kind": "FVAL", "u": bits, "text": val}
    if reg == "cr":
        return {"reg": reg, "kind": "CR", "u": parse_int(val) & 0xFFFFFFFF}
    if reg in ("xer_ca", "xer_ov", "xer_so"):
        return {"reg": reg, "kind": reg.upper(), "u": parse_int(val)}
    if reg.startswith("r") and reg[1:].isdigit():
        return {"reg": reg, "kind": "GPR", "u": parse_int(val)}
    if reg == "ctr":
        return {"reg": reg, "kind": "CTR", "u": parse_int(val)}
    raise SystemExit(f"unsupported register {reg}")


def parse_nm(path):
    syms = {}
    for line in open(path):
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tT":
            syms[parts[2]] = int(parts[0], 16)
    return syms


def parse_generated(path):
    src = open(path).read()
    funcs = {}
    for name, body in FUNC_RE.findall(src):
        funcs[name] = {"body": body, "calls": set(CALL_RE.findall(body)) - {name},
                       "trap": "__builtin_debugtrap();" in body or "PPC_CALL_UNKNOWN(" in body}
    return funcs


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_regs(regs):
    out = []
    for r in regs:
        w = r.get("w", [0, 0, 0, 0])
        out.append("{%s, RK_%s, 0x%XULL, {0x%X, 0x%X, 0x%X, 0x%X}}" % (
            c_str(r["reg"]), r["kind"], r.get("u", 0), *w))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True, help="build dir written by cpu/tools/build_corpus.py")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    manifest = json.load(open(os.path.join(args.corpus, "manifest.json")))
    os.makedirs(args.out, exist_ok=True)
    static_results, units = [], []
    all_decls = []

    for fx in manifest["fixtures"]:
        set_, stem = fx["set"], fx["stem"]
        imports = {}
        tests = parse_fixture(fx["source"], imports)
        base_id = f"{set_}/{stem}"
        if fx["status"] != "generated":
            for t in tests:
                static_results.append({"id": f"{base_id}/{t['name']}", "status": "BLOCKED", "stage": "assemble",
                                       "reason": "fixture does not assemble with upstream binutils (VMX128/Xenon-only encodings)"})
            continue
        wdir = os.path.join(args.corpus, set_, stem)
        syms = parse_nm(os.path.join(wdir, stem + ".nm"))
        funcs = parse_generated(os.path.join(wdir, "gen", stem + ".cpp"))
        unimpl = set(fx.get("unimplemented_functions", []))
        rc_addrs = [w["addr"] for w in fx.get("rc_bit_warnings", [])]
        func_addr = {n: int(n[len(stem) + 1:], 16) for n in funcs}
        starts = sorted(func_addr.values())

        def owner(addr):
            best = None
            for s in starts:
                if s <= addr:
                    best = s
            return best

        flagged = {a: "generator reported unimplemented instructions" for a in unimpl}
        for a in rc_addrs:
            o = owner(a)
            if o is not None:
                flagged.setdefault(o, "generator skipped a record-form CR update")
        for n, f in funcs.items():
            if f["trap"]:
                flagged.setdefault(func_addr[n], "generator emitted __builtin_debugtrap (unsupported form)")

        def closure_problem(name, seen):
            if name in seen:
                return None
            seen.add(name)
            a = func_addr[name]
            if a in flagged:
                return f"{name}: {flagged[a]}"
            for c in funcs[name]["calls"]:
                if c in funcs:
                    p = closure_problem(c, seen)
                    if p:
                        return p
            return None

        cases, externs = [], set()
        for idx, t in enumerate(tests):
            tid = f"{base_id}/{t['name']}"
            if t["name"] not in syms:
                static_results.append({"id": tid, "status": "FAIL", "stage": "harness", "reason": "label not in symbol table"})
                continue
            addr = syms[t["name"]]
            fname = f"{stem}_{addr:X}"
            if fx.get("relocations"):
                static_results.append({"id": tid, "status": "BLOCKED", "stage": "assemble",
                                       "reason": "unlinked object carries relocations; fixture needs a link step"})
                continue
            if fname not in funcs:
                static_results.append({"id": tid, "status": "FAIL", "stage": "generate",
                                       "reason": f"no generated function at 0x{addr:X}"})
                continue
            prob = closure_problem(fname, set())
            if prob:
                static_results.append({"id": tid, "status": "FAIL", "stage": "generate", "reason": prob})
                continue
            externs.add(fname)
            sym = f"c{idx}"
            lines = []
            for kind, regs in (("in", t["ins"]), ("out", t["outs"])):
                body = emit_regs(regs)
                lines.append(f"static const RegVal {sym}_{kind}[] = {{{', '.join(body) or '{}'}}};")
            for kind, mems in (("min", t["mem_in"]), ("mout", t["mem_out"])):
                entries = []
                for j, (a, data) in enumerate(mems):
                    lines.append(f"static const uint8_t {sym}_{kind}{j}[] = {{{', '.join('0x%02X' % b for b in data)}}};")
                    entries.append(f"{{0x{a:X}u, {sym}_{kind}{j}, {len(data)}u}}")
                lines.append(f"static const MemVal {sym}_{kind}[] = {{{', '.join(entries) or '{}'}}};")
            cases.append(("\n".join(lines),
                          f"{{{c_str(tid)}, {fname}, 0x{addr:X}u, "
                          f"{sym}_in, {len(t['ins'])}, {sym}_out, {len(t['outs'])}, "
                          f"{sym}_min, {len(t['mem_in'])}, {sym}_mout, {len(t['mem_out'])}, "
                          f"{c_str(t['expect_fatal']) if t['expect_fatal'] else 'nullptr'}, "
                          f"{c_str(t['program']) if t['program'] else 'nullptr'}, "
                          f"{c_str(t['gfx_expect']) if t['gfx_expect'] else 'nullptr'}}}"))
        # Object fixtures may start at zero; their entry is called directly
        # through TestCase.fn. Null remains invalid for indirect dispatch.
        # Keep every declaration and test case, but register only nonzero
        # addresses under the production function-table contract.
        all_funcs = sorted(funcs, key=lambda n: func_addr[n])
        dispatch_funcs = [n for n in all_funcs if func_addr[n] != 0]
        all_decls += all_funcs
        unit = f"{set_}_{stem}".replace("-", "_")
        src = ["// Generated by tests/cpu/gen_harness.py. Do not edit.",
               '#include "cpu_harness.h"', ""]
        src += [f"PPC_EXTERN_FUNC({n});" for n in all_funcs]
        src.append("")
        # Import bindings: the helper standing for an import thunk has a weak
        # generated body; this strong definition forwards to the runtime's
        # __imp__<name> (runtime/src/import_thunks.cpp), as a XEX loader would.
        for helper, (module, iname) in sorted(imports.items()):
            if helper not in syms:
                raise SystemExit(f"{fx['source']}: IMPORT helper {helper} not in symbol table")
            hname = f"{stem}_{syms[helper]:X}"
            if hname not in funcs:
                raise SystemExit(f"{fx['source']}: no generated function for IMPORT helper {helper}")
            target = f"rcomp_hle_{iname}" if module == "hle" else f"__imp__{iname}"
            what = f"HLE {iname}" if module == "hle" else f"import {module}!{iname}"
            src.append(f"// {what} bound at guest 0x{syms[helper]:X} ({helper})")
            src.append(f"PPC_EXTERN_FUNC({target});")
            src.append(f"PPC_FUNC({hname}) {{ {target}(ctx, base); }}")
        src.append("")
        for decl, _ in cases:
            src.append(decl)
        src.append(f"static const TestCase cases[] = {{{', '.join(c for _, c in cases) or '{}'}}};")
        src.append(f"static const rcomp::FuncEntry funcs[] = {{"
                   + (", ".join(f"{{0x{func_addr[n]:X}u, {n}, {c_str(n)}}}" for n in dispatch_funcs) or "{}") + "};")
        src.append(f"extern const TestUnit unit_{unit} = {{{c_str(base_id)}, cases, {len(cases)}, funcs, {len(dispatch_funcs)}}};")
        with open(os.path.join(args.out, f"cases_{unit}.cpp"), "w") as f:
            f.write("\n".join(src) + "\n")
        units.append(unit)

    idx = ['// Generated by tests/cpu/gen_harness.py. Do not edit.', '#include "cpu_harness.h"']
    idx += [f"extern const TestUnit unit_{u};" for u in units]
    idx.append("const TestUnit* const kUnits[] = {" + ", ".join(f"&unit_{u}" for u in units) + "};")
    idx.append(f"const int kUnitCount = {len(units)};")
    with open(os.path.join(args.out, "cases_index.cpp"), "w") as f:
        f.write("\n".join(idx) + "\n")
    # XenonRecomp test mode emits no forward declarations, so a direct call to
    # a function defined later in the same file does not compile. This header
    # is force-included in front of generated TUs (never edits them).
    with open(os.path.join(args.out, "generated_decls.h"), "w") as f:
        f.write("// Generated by tests/cpu/gen_harness.py. Do not edit.\n#pragma once\n#include <stdint.h>\n"
                "struct PPCContext;\n")
        f.writelines(f"void {n}(PPCContext& __restrict ctx, uint8_t* base);\n" for n in all_decls)
    with open(os.path.join(args.out, "cases.json"), "w") as f:
        json.dump({"static_results": static_results, "units": units}, f, indent=1)
    print(json.dumps({"units": len(units), "static_results": len(static_results)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
