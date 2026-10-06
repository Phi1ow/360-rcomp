#!/usr/bin/env python3
"""Prepares the SPIR-V directory of one draw resolution scale for the host test from the dump shaders that the title logged (extract_dump_spirv.py; the title ran at scale 3):
  - the dump shaders of the scale: the logged ones have the 80x16 tile of the EDRAM scaled by 3 (240 x 48 samples, 11,520 per tile) as constants; those three constants are replaced;
  - dump_c32f_<m>x.spv for a multisampled 32-bit float render target that the title did not log, derived from the logged 8:8:8:8 dump shader of the same MSAA (the title's dump
    shader differs only in the source image, sampled as unsigned integers (the float kept as bits), and in the value stored: the sample as it is instead of packed to 8:8:8:8).
    The same derivation applied to the 1x 8:8:8:8 shader must reproduce the title's logged c32f_1x dump shader: `--check` runs it and compares the two function bodies;
  - the precompiled copy shaders (copy.spv, resolve_full_32bpp_scaled_cs.spv, resolve_full_64bpp_scaled_cs.spv) are copied.
usage: make_dump_variants.py <dir with the logged dump_*.spv and the copy shaders> <out dir> <scale 1..3> [--check]
spirv-dis / spirv-as: from PATH, or the SPIRV_DIS / SPIRV_AS environment variables."""
import difflib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SPIRV_DIS = os.environ.get("SPIRV_DIS", "spirv-dis")
SPIRV_AS = os.environ.get("SPIRV_AS", "spirv-as")


def disassemble(path):
    return subprocess.run([SPIRV_DIS, "--no-header", "--no-color", str(path)], capture_output=True, text=True, check=True).stdout


def assemble(text, out):
    tmp = Path(str(out) + ".spvasm")
    tmp.write_text(text, encoding="utf-8")
    subprocess.run([SPIRV_AS, "--target-env", "spv1.0", str(tmp), "-o", str(out)], check=True)
    tmp.unlink()


def rescale(text, scale):
    """The logged dump shaders are for scale 3: tile 240x48 samples, 11520 samples per tile."""
    if scale == 3:
        return text
    values = {"240": str(80 * scale), "48": str(16 * scale), "11520": str(1280 * scale * scale)}
    count = 0

    def sub(m):
        nonlocal count
        count += 1
        return f"{m.group(1)}{values[m.group(2)]}"

    text = re.sub(r"(%\S+ = OpConstant %uint )(240|48|11520)\b", sub, text)
    assert count == 3, f"expected the 3 tile constants of the dump shader, found {count}"
    return text


def derive_c32f(text):
    """8:8:8:8 float image -> 32-bit float stored as bits: the image is read as unsigned integers and the sample is stored as it is."""
    lines = text.split("\n")
    image_found = False
    for i, line in enumerate(lines):
        if re.match(r"\s*%\S+ = OpTypeImage %float 2D 0 0 \d 1 Unknown", line):
            lines[i] = line.replace("OpTypeImage %float", "OpTypeImage %uint")
            image_found = True
    assert image_found, "no float sampled image type"
    fetch_index = next(i for i, l in enumerate(lines) if "OpImageFetch %v4float" in l)
    fetched = re.match(r"\s*(%\S+) = OpImageFetch", lines[fetch_index]).group(1)
    lines[fetch_index] = lines[fetch_index].replace("OpImageFetch %v4float", "OpImageFetch %v4uint")
    store_index = next(i for i, l in enumerate(lines) if re.match(r"\s*OpStore ", l))
    stored = re.match(r"\s*OpStore \S+ (%\S+)", lines[store_index]).group(1)
    defs = {}
    for i, l in enumerate(lines):
        m = re.match(r"\s*(%\S+) = Op\w+", l)
        if m:
            defs[m.group(1)] = i
    memo = {}

    def depends_on_fetch(name):
        if name == fetched:
            return True
        if name not in memo:
            memo[name] = False
            index = defs.get(name)
            if index is not None and fetch_index < index < store_index:
                operands = re.findall(r"%\S+", lines[index].split("=", 1)[1])
                memo[name] = any(depends_on_fetch(o) for o in operands)
        return memo[name]

    chain = sorted(i for name, i in defs.items() if fetch_index < i < store_index and depends_on_fetch(name))
    assert chain, "no conversion chain after the fetch"
    for i in chain[1:]:
        lines[i] = None
    lines[chain[0]] = f"         {stored} = OpCompositeExtract %uint {fetched} 0"
    text = "\n".join(l for l in lines if l is not None)
    if "%v4uint = OpTypeVector %uint 4" not in text:
        text, n = re.subn(r"(\n\s*%v3uint = OpTypeVector %uint 3\n)", r"\1     %v4uint = OpTypeVector %uint 4\n", text, count=1)
        assert n == 1, "no %v3uint to place %v4uint after"
    return text


def function_body(text):
    """The instructions of the function with constants resolved to (type, value) and the other ids erased, for the comparison of two disassemblies."""
    constants = dict(re.findall(r"(%\S+) = OpConstant (%\S+ \S+)", text))
    lines = text.split("\n")
    start = next(i for i, l in enumerate(lines) if "= OpFunction " in l or l.strip().startswith("OpFunction"))
    body = []
    for l in lines[start:]:
        l = l.strip()
        if not l:
            continue
        l = l.split("=", 1)[-1].strip()
        body.append(re.sub(r"%\S+", lambda m: "C(" + constants[m.group(0)] + ")" if m.group(0) in constants else "%", l))
    return body


def main():
    src, out, scale = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])
    check = "--check" in sys.argv
    out.mkdir(parents=True, exist_ok=True)
    for name in ("copy.spv", "resolve_full_32bpp_scaled_cs.spv", "resolve_full_64bpp_scaled_cs.spv", "resolve_fast_32bpp_1x2xmsaa_scaled_cs.spv"):
        if (src / name).exists() and (src / name).resolve() != (out / name).resolve():
            shutil.copyfile(src / name, out / name)
    if not (out / "copy.spv").exists() and (out / "resolve_fast_32bpp_1x2xmsaa_scaled_cs.spv").exists():
        shutil.copyfile(out / "resolve_fast_32bpp_1x2xmsaa_scaled_cs.spv", out / "copy.spv")  # the fast copy of the earlier host test
    written = 0
    for dump in sorted(src.glob("dump_*.spv")):
        if scale == 3 and out.resolve() == src.resolve():
            continue
        assemble(rescale(disassemble(dump), scale), out / dump.name)
        written += 1
    for m in ("1x", "2x", "4x"):
        base = src / f"dump_c8888_{m}.spv"
        if not base.exists():
            continue
        derived = derive_c32f(rescale(disassemble(base), scale))
        if (src / f"dump_c32f_{m}.spv").exists():
            if check:
                logged = function_body(rescale(disassemble(src / f"dump_c32f_{m}.spv"), scale))
                tmp = out / f"derived_c32f_{m}.spv"
                assemble(derived, tmp)
                ours = function_body(disassemble(tmp))
                tmp.unlink()
                same = logged == ours
                print(f"check dump_c32f_{m}: the title's logged shader and the one derived from c8888_{m} have", "the same function body" if same else "DIFFERENT function bodies")
                if not same:
                    print("\n".join(difflib.unified_diff(logged, ours, "logged", "derived", lineterm="")))
                    sys.exit(1)
            continue
        assemble(derived, out / f"dump_c32f_{m}.spv")
        written += 1
        print(f"derived dump_c32f_{m}.spv from dump_c8888_{m}.spv")
    print(f"{written} dump shaders written to {out} for scale {scale}")


main()
