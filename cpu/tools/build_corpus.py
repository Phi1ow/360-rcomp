#!/usr/bin/env python3
"""PPC fixture corpus -> XenonRecomp-generated C++ (host step, Agent 1).

For every fixture `<set>/<stem>.s`:
  1. assemble with GNU binutils for 32-bit big-endian PowerPC (Xenia's flags
     minus -mvmx128, which upstream binutils does not implement);
  2. record the symbol table (nm) and a disassembly (objdump) for traceability;
  3. run the *real* XenonRecomp in test mode on a directory holding only that
     object, so every generator diagnostic is attributable to one fixture;
  4. write manifest.json with tool versions, exact commands, return codes and
     sha256 of every input and output.

Nothing here edits generated C++. The generated main.cpp of test mode is kept
for provenance under the name upstream_main.cpp.txt and never compiled: the
harness is produced independently by tests/cpu/gen_harness.py.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

AS_FLAGS = ["-a32", "-be", "-mregnames", "-mpower7", "-maltivec", "-mvsx", "-R"]
# Fallback for the Cell/Xenon-only lvlx/lvrx/stvlx/stvrx family, which power7
# mode rejects. Accepted only if the assembler prints nothing at all.
AS_FLAGS_CELL = ["-a32", "-be", "-mregnames", "-mcell", "-maltivec", "-R"]
# Last fallback: binutils 2.24 + Xenia's VMX128 patch
# (cpu/tools/build_binutils_vmx128.sh), with Xenia's exact gentests flags.
AS_FLAGS_VMX128 = ["-a32", "-be", "-mregnames", "-mpower7", "-maltivec", "-mvsx", "-mvmx128", "-R"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    return {"cmd": cmd, "rc": p.returncode, "stdout": p.stdout, "stderr": p.stderr}


def tool_version(tool):
    try:
        return subprocess.run([tool, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except Exception as e:  # noqa: BLE001
        return f"unavailable: {e}"


def git_info(path):
    def g(*a):
        return subprocess.run(["git", "-C", path, *a], capture_output=True, text=True).stdout.strip()
    return {"commit": g("rev-parse", "HEAD"), "dirty_files": len([l for l in g("status", "--porcelain").splitlines() if l])}


UNIMPL_RE = re.compile(r"Function ([0-9A-F]+) in (\S+) has unimplemented instructions")
RCBIT_RE = re.compile(r"(\S+) at ([0-9A-F]+) has RC bit enabled but no comparison was generated")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", action="append", required=True, help="name=dir_with_.s_files")
    ap.add_argument("--out", required=True)
    ap.add_argument("--xenonrecomp", required=True)
    ap.add_argument("--xenonrecomp-src", required=True)
    ap.add_argument("--prefix", default="powerpc-linux-gnu-")
    ap.add_argument("--only", help="regex on stem")
    ap.add_argument("--patches", help="directory of generator patches applied to the build copy")
    ap.add_argument("--vmx128-as", help="VMX128-capable assembler (fallback)")
    args = ap.parse_args()

    tools = {k: args.prefix + k for k in ("as", "nm", "objdump")}
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    manifest = {
        "generator": {
            "binary": os.path.abspath(args.xenonrecomp),
            "binary_sha256": sha256(args.xenonrecomp),
            "source": git_info(args.xenonrecomp_src),
            "patches": {f: sha256(os.path.join(args.patches, f)) for f in sorted(os.listdir(args.patches))
                        if f.endswith(".patch")} if args.patches else {},
        },
        "tools": {k: tool_version(v) for k, v in tools.items()},
        "as_flags": AS_FLAGS,
        "vmx128_as": (tool_version(args.vmx128_as) + " sha256:" + sha256(args.vmx128_as)) if args.vmx128_as else None,
        "sets": {},
        "fixtures": [],
    }

    for spec in args.set:
        name, src = spec.split("=", 1)
        src = os.path.abspath(src)
        info = {"path": src}
        if os.path.isdir(os.path.join(src, ".git")) or subprocess.run(
                ["git", "-C", src, "rev-parse"], capture_output=True).returncode == 0:
            info.update(git_info(src))
        manifest["sets"][name] = info
        for fn in sorted(os.listdir(src)):
            if not fn.endswith(".s"):
                continue
            stem = fn[:-2]
            if args.only and not re.search(args.only, stem):
                continue
            s_path = os.path.join(src, fn)
            wdir = os.path.join(out, name, stem)
            if os.path.isdir(wdir):
                shutil.rmtree(wdir)
            bindir, gendir = os.path.join(wdir, "bin"), os.path.join(wdir, "gen")
            os.makedirs(bindir)
            os.makedirs(gendir)
            obj = os.path.join(bindir, stem + ".o")
            fx = {"set": name, "stem": stem, "source": s_path, "source_sha256": sha256(s_path), "steps": {}}
            r = run([tools["as"], *AS_FLAGS, "-o", obj, s_path])
            if r["rc"] != 0:
                first = {k: r[k] for k in ("cmd", "rc", "stderr")}
                r = run([tools["as"], *AS_FLAGS_CELL, "-o", obj, s_path])
                if r["stderr"].strip():
                    r["rc"] = r["rc"] or 1
                fx["steps"]["assemble_power7_attempt"] = first
                if r["rc"] != 0 and args.vmx128_as:
                    fx["steps"]["assemble_cell_attempt"] = {k: r[k] for k in ("cmd", "rc", "stderr")}
                    r = run([args.vmx128_as, *AS_FLAGS_VMX128, "-o", obj, s_path])
                    if r["stderr"].strip():
                        r["rc"] = r["rc"] or 1
            fx["steps"]["assemble"] = {k: r[k] for k in ("cmd", "rc", "stderr")}
            if r["rc"] != 0:
                fx["status"] = "assemble_failed"
                manifest["fixtures"].append(fx)
                continue
            fx["object_sha256"] = sha256(obj)
            r = run([tools["nm"], "--numeric-sort", obj])
            with open(os.path.join(wdir, stem + ".nm"), "w") as f:
                f.write(r["stdout"])
            r2 = run([tools["objdump"], "-Mpower7", "-d", "-r", "-EB", obj])
            with open(os.path.join(wdir, stem + ".dis"), "w") as f:
                f.write("\n".join(r2["stdout"].splitlines()[3:]) + "\n")
            # Relocations would mean the unlinked object holds placeholder
            # values XenonRecomp would translate literally.
            relocs = run([tools["objdump"], "-r", obj])["stdout"]
            fx["relocations"] = [l for l in relocs.splitlines() if re.match(r"^[0-9a-f]{8} ", l)]
            r = run([args.xenonrecomp, bindir, gendir])
            log = r["stdout"] + r["stderr"]
            with open(os.path.join(wdir, "xenonrecomp.log"), "w") as f:
                f.write(log)
            fx["steps"]["xenonrecomp"] = {"cmd": r["cmd"], "rc": r["rc"]}
            if r["rc"] != 0:
                fx["status"] = "generate_failed"
                manifest["fixtures"].append(fx)
                continue
            up_main = os.path.join(gendir, "main.cpp")
            if os.path.exists(up_main):
                os.replace(up_main, os.path.join(wdir, "upstream_main.cpp.txt"))
            fx["unimplemented_functions"] = sorted({int(a, 16) for a, s in UNIMPL_RE.findall(log) if s == stem})
            fx["rc_bit_warnings"] = [{"insn": i, "addr": int(a, 16)} for i, a in RCBIT_RE.findall(log)]
            fx["generated"] = {f: sha256(os.path.join(gendir, f)) for f in sorted(os.listdir(gendir))}
            fx["status"] = "generated"
            manifest["fixtures"].append(fx)

    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    counts = {}
    for fx in manifest["fixtures"]:
        counts[fx["status"]] = counts.get(fx["status"], 0) + 1
    print(json.dumps(counts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
