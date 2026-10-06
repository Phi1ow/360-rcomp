#!/usr/bin/env python3
"""R-comp Xenos shader-corpus manifest + PS5_Vulkan capability gate.

For every *.hlsl in a corpus directory:
  1. record source path + SHA-256,
  2. compile it with DXC using the exact option set XenosRecomp passes
     (XenosRecomp/dxc_compiler.cpp @990d03b2: -T vs_6_0|ps_6_0, -HV 2021,
     -all-resources-bound, -spirv, -fvk-use-dx-layout, -fvk-invert-y (VS only),
     -Qstrip_debug; optional extra -D defines per profile),
  3. record SPIR-V SHA-256, version, OpCapability / OpExtension / OpMemoryModel,
     descriptor bindings, push-constant use, spec constants,
  4. run `spirv-val --target-env <env>` (default vulkan1.0: PS5_Vulkan reports
     VK_API_VERSION_1_0, driver/ps5vk_private.h:62),
  5. evaluate two gates:
     - driver_refusal: the exact pre-compile refusal PS5_Vulkan implements
       (driver/ps5vk_pipeline.c:606-619 + 674-697: addressing model
       PhysicalStorageBuffer64 or capability PhysicalStorageBufferAddressesEXT),
     - feature_gate: capabilities that need a Vulkan feature/extension the
       PS5_Vulkan physical device does not advertise
       (driver/ps5vk_physical_device.c:51-72: only robustBufferAccess,
       samplerAnisotropy, dualSrcBlend; device extension KHR_swapchain only).

Never edits SPIR-V. A FAIL here means the *generator* or its prelude must
change, not the binary.

Usage:
  shader_manifest.py --corpus DIR --out DIR --dxc PATH [--dxc-lib DIR]
                     [--target-env vulkan1.0] [--define NAME ...]
Exit code: 0 if every shader compiles, validates and passes all gates
           (with --check-expectations: iff every status equals its
           '// rcomp-expect:' annotation); 1 otherwise; 2 on usage/tool errors.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# Capabilities whose use needs a feature/extension PS5_Vulkan does not expose.
# Evidence for each "not exposed": ps5vk_physical_device.c:51-72 (feature set) and
# docs/M5_REFERENCE.md (rungs 1.1-1.4 not reached).
NEEDS_UNEXPOSED = {
    "Int64": "shaderInt64 (feature off)",
    "Float64": "shaderFloat64 (feature off)",
    "Int16": "shaderInt16 (feature off)",
    "Float16": "shaderFloat16 / VK_KHR_shader_float16_int8 (not exposed)",
    "Int8": "shaderInt8 / VK_KHR_shader_float16_int8 (not exposed)",
    "PhysicalStorageBufferAddresses": "bufferDeviceAddress (not exposed; README 'Buffer device address is not advertised')",
    "PhysicalStorageBufferAddressesEXT": "bufferDeviceAddress (driver refuses by name)",
    "RuntimeDescriptorArray": "descriptorIndexing runtimeDescriptorArray (VK_EXT_descriptor_indexing not exposed)",
    "RuntimeDescriptorArrayEXT": "descriptorIndexing runtimeDescriptorArray (not exposed)",
    "ShaderNonUniform": "descriptorIndexing non-uniform indexing (not exposed)",
    "ShaderNonUniformEXT": "descriptorIndexing non-uniform indexing (not exposed)",
    "SampledImageArrayNonUniformIndexing": "descriptorIndexing (not exposed)",
    "SampledImageArrayDynamicIndexing": "shaderSampledImageArrayDynamicIndexing (feature off)",
    "UniformBufferArrayDynamicIndexing": "shaderUniformBufferArrayDynamicIndexing (feature off)",
    "StorageBufferArrayDynamicIndexing": "shaderStorageBufferArrayDynamicIndexing (feature off)",
    "ClipDistance": "shaderClipDistance (feature off, maxClipDistances=0)",
    "CullDistance": "shaderCullDistance (feature off)",
    "Geometry": "geometryShader (feature off)",
    "Tessellation": "tessellationShader (feature off)",
    "ImageGatherExtended": "shaderImageGatherExtended (feature off)",
    "SampleRateShading": "sampleRateShading (feature off at 9639c41; lowered by compiler per ps5vk_pipeline.c:594)",
    "DemoteToHelperInvocation": "shaderDemoteToHelperInvocation (1.3 feature, not exposed)",
    "VulkanMemoryModel": "vulkanMemoryModel (not exposed)",
    "StorageBuffer16BitAccess": "16-bit storage (not exposed)",
}
# Extensions: PS5_Vulkan exposes no device extension that maps to SPIR-V
# extensions (only VK_KHR_swapchain). Any OpExtension is therefore a gap unless
# it is core-equivalent in SPIR-V 1.0 (none of the ones below are).
DRIVER_REFUSED_ADDRESSING = "PhysicalStorageBuffer64"
# Draw-time refusals at PS5_Vulkan@9639c41 that a shader alone already implies.
#   ps5vk_draw.c:947-955  more than one colour attachment is refused (v0-mrt)
#   ps5vk_draw.c:1341-1350 image views other than 2D / 2D array / cube are refused
DRIVER_REFUSED_CAPS = {"PhysicalStorageBufferAddressesEXT"}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def detect_stage(path: Path, text: str) -> str:
    m = re.search(r"//\s*rcomp-stage:\s*(vs|ps)", text)
    if m:
        return m.group(1)
    if '[shader("pixel")]' in text:
        return "ps"
    if '[shader("vertex")]' in text:
        return "vs"
    name = path.name.lower()
    if ".ps." in name or name.startswith("ps_") or "_ps_" in name:
        return "ps"
    if ".vs." in name or name.startswith("vs_") or "_vs_" in name:
        return "vs"
    raise ValueError(f"cannot determine stage of {path}")


def dxc_args(stage: str, defines, include_dirs):
    # Mirrors XenosRecomp/dxc_compiler.cpp:24-58 for compileSpirv == true.
    args = ["-T", "ps_6_0" if stage == "ps" else "vs_6_0", "-HV", "2021",
            "-all-resources-bound", "-spirv", "-fvk-use-dx-layout"]
    if stage == "vs":
        args.append("-fvk-invert-y")
    args.append("-Qstrip_debug")
    for d in defines:
        args.append(f"-D{d}")
    for inc in include_dirs:
        args += ["-I", str(inc)]
    return args


def run(cmd, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return p.returncode, p.stdout, p.stderr


def analyse_disassembly(dis: str):
    caps, exts, bindings, spec = [], [], [], []
    memory_model = None
    image_dims = set()
    output_locations = set()
    output_vars = set()
    version = None
    push_constant = False
    names = {}
    for line in dis.splitlines():
        s = line.strip()
        m = re.match(r"; Version: (\S+)", s)
        if m:
            version = m.group(1)
        m = re.match(r"OpCapability (\w+)", s)
        if m:
            caps.append(m.group(1))
        m = re.match(r'OpExtension "([^"]+)"', s)
        if m:
            exts.append(m.group(1))
        m = re.match(r"OpMemoryModel (\w+) (\w+)", s)
        if m:
            memory_model = [m.group(1), m.group(2)]
        m = re.match(r'OpName (%\S+) "([^"]*)"', s)
        if m:
            names[m.group(1)] = m.group(2)
        m = re.match(r"OpDecorate (%\S+) (DescriptorSet|Binding|SpecId) (\d+)", s)
        if m:
            if m.group(2) == "SpecId":
                spec.append({"id": m.group(1), "spec_id": int(m.group(3))})
            else:
                bindings.append((m.group(1), m.group(2), int(m.group(3))))
        m = re.match(r"%\S+ = OpTypeImage %\S+ (\w+) ", s)
        if m:
            image_dims.add(m.group(1))
        m = re.match(r"(%\S+) = OpVariable %\S+ Output", s)
        if m:
            output_vars.add(m.group(1))
        m = re.match(r"OpDecorate (%\S+) Location (\d+)", s)
        if m:
            output_locations.add((m.group(1), int(m.group(2))))
        if "PushConstant" in s and "OpVariable" in s:
            push_constant = True
    merged = {}
    for var, kind, val in bindings:
        merged.setdefault(var, {"var": names.get(var, var)})[kind] = val
    return {
        "spirv_version": version,
        "capabilities": caps,
        "extensions": exts,
        "memory_model": memory_model,
        "descriptor_bindings": sorted(merged.values(), key=lambda b: (b.get("DescriptorSet", -1), b.get("Binding", -1))),
        "push_constant_block": push_constant,
        "spec_constants": spec,
        "image_dims": sorted(image_dims),
        "output_locations": sorted(loc for var, loc in output_locations if var in output_vars),
    }


def dynamic_descriptor_array_indexing(dis: str):
    """Vulkan 1.0 requires constant indices into arrays of images/samplers
    unless shader*ArrayDynamicIndexing is enabled, and DXC does not declare a
    capability for it, so detect the access pattern itself: an OpAccessChain
    whose base is a UniformConstant array-of-descriptors variable and whose
    index is not an OpConstant."""
    types, ptr_pointee, consts, arrays, hits, names = {}, {}, set(), set(), [], {}
    for line in dis.splitlines():
        s = line.strip()
        m = re.match(r'OpName (%\S+) "([^"]*)"', s)
        if m:
            names[m.group(1)] = m.group(2)
        m = re.match(r"(%\S+) = (OpType\w+)(.*)", s)
        if m:
            types[m.group(1)] = (m.group(2), m.group(3).split())
            if m.group(2) == "OpTypePointer":
                ptr_pointee[m.group(1)] = m.group(3).split()[1]
        m = re.match(r"(%\S+) = OpConstant\w* ", s)
        if m:
            consts.add(m.group(1))
        m = re.match(r"(%\S+) = OpVariable (%\S+) UniformConstant", s)
        if m:
            pointee = types.get(ptr_pointee.get(m.group(2), ""), (None, []))
            if pointee[0] in ("OpTypeArray", "OpTypeRuntimeArray"):
                elem = types.get(pointee[1][0], (None, []))[0]
                if elem in ("OpTypeImage", "OpTypeSampler", "OpTypeSampledImage"):
                    arrays.add(m.group(1))
        m = re.match(r"%\S+ = OpAccessChain %\S+ (%\S+) (%\S+)", s)
        if m and m.group(1) in arrays and m.group(2) not in consts:
            hits.append(m.group(1))
    return sorted({names.get(h, h) for h in hits})


def gates(info):
    refusal = []
    if info["memory_model"] and info["memory_model"][0] == DRIVER_REFUSED_ADDRESSING:
        refusal.append(f"addressing model {DRIVER_REFUSED_ADDRESSING} (ps5vk_pipeline.c:674-683)")
    for c in info["capabilities"]:
        if c in DRIVER_REFUSED_CAPS:
            refusal.append(f"capability {c} (ps5vk_pipeline.c:615-619)")
    feature = [f"{c}: {NEEDS_UNEXPOSED[c]}" for c in info["capabilities"] if c in NEEDS_UNEXPOSED]
    feature += [f"extension {e}: no matching device extension exposed (only VK_KHR_swapchain)" for e in info["extensions"]]
    for var in info.get("dynamic_descriptor_array_indexing", []):
        feature.append(f"dynamic index into descriptor array {var}: shaderSampledImageArrayDynamicIndexing "
                       f"(feature off, ps5vk_physical_device.c:51-67)")
    draw = []
    for dim in info["image_dims"]:
        if dim in ("1D", "3D"):
            draw.append(f"samples a {dim} image: view type refused at draw (ps5vk_draw.c:1341-1350)")
    if info.get("stage") == "ps" and len(info["output_locations"]) > 1:
        draw.append(f"writes {len(info['output_locations'])} colour outputs: >1 colour attachment refused (ps5vk_draw.c:947-955)")
    return refusal, feature, draw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--dxc", required=True)
    ap.add_argument("--dxc-lib", default=None)
    ap.add_argument("--target-env", default="vulkan1.0")
    ap.add_argument("--define", action="append", default=[])
    ap.add_argument("--include", action="append", default=[], type=Path)
    ap.add_argument("--check-expectations", action="store_true",
                    help="each source carries '// rcomp-expect: PASS|FAIL'; exit 0 iff all match")
    ap.add_argument("--corpus-kind", default="self-authored",
                    help="recorded in the manifest; 'xenos-real' is BLOCKED in this project")
    a = ap.parse_args()

    if a.corpus_kind == "xenos-real":
        print("BLOCKED: no authorized Xbox 360 shader corpus in this environment", file=sys.stderr)
        return 2
    sources = sorted(a.corpus.rglob("*.hlsl"))
    if not sources:
        print(f"no *.hlsl under {a.corpus}", file=sys.stderr)
        return 2
    a.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if a.dxc_lib:
        env["LD_LIBRARY_PATH"] = a.dxc_lib + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    rc, ver, _ = run([a.dxc, "--version"], env)
    rc2, val_ver, _ = run(["spirv-val", "--version"])
    manifest = {
        "tool": "gpu/xenos/tools/shader_manifest.py",
        "corpus": str(a.corpus), "corpus_kind": a.corpus_kind,
        "dxc": {"path": a.dxc, "version": ver.strip(), "sha256": sha256(Path(a.dxc).read_bytes())},
        "spirv_val": val_ver.strip().splitlines()[0] if val_ver else None,
        "target_env": a.target_env,
        "defines": a.define,
        "shaders": [],
    }
    worst = 0
    for src in sources:
        text = src.read_text(errors="replace")
        entry = {"source": str(src.relative_to(a.corpus)), "source_sha256": sha256(src.read_bytes())}
        try:
            stage = detect_stage(src, text)
        except ValueError as e:
            entry.update(status="FAIL", error=str(e))
            manifest["shaders"].append(entry)
            worst = 1
            continue
        spv = a.out / (src.stem + ".spv")
        args = dxc_args(stage, a.define, a.include)
        entry.update(stage=stage, dxc_args=args)
        crc, _, cerr = run([a.dxc, *args, "-Fo", str(spv), str(src)], env)
        entry["dxc_rc"] = crc
        if crc != 0:
            entry.update(status="FAIL", dxc_stderr=cerr.strip()[-4000:])
            manifest["shaders"].append(entry)
            worst = 1
            continue
        if cerr.strip():
            entry["dxc_warnings"] = cerr.strip()[-2000:]
        blob = spv.read_bytes()
        entry["spirv_sha256"] = sha256(blob)
        entry["spirv_bytes"] = len(blob)
        drc, dis, derr = run(["spirv-dis", "--raw-id", str(spv)])
        if drc != 0:
            drc, dis, derr = run(["spirv-dis", str(spv)])
        (a.out / (src.stem + ".spvasm")).write_text(dis)
        entry.update(analyse_disassembly(dis))
        entry["dynamic_descriptor_array_indexing"] = dynamic_descriptor_array_indexing(dis)
        vrc, vout, verr = run(["spirv-val", "--target-env", a.target_env, str(spv)])
        entry["spirv_val"] = {"target_env": a.target_env, "rc": vrc, "message": (vout + verr).strip()[-2000:]}
        refusal, feature, draw = gates(entry)
        entry["ps5vk_driver_refusal"] = refusal
        entry["ps5vk_feature_gaps"] = feature
        entry["ps5vk_draw_time_refusals"] = draw
        ok = vrc == 0 and not refusal and not feature and not draw
        entry["status"] = "PASS" if ok else "FAIL"
        if not ok:
            worst = 1
        manifest["shaders"].append(entry)

    mismatches = 0
    if a.check_expectations:
        for e in manifest["shaders"]:
            src_text = (a.corpus / e["source"]).read_text(errors="replace")
            m = re.search(r"//\s*rcomp-expect:\s*(PASS|FAIL)", src_text)
            e["expected"] = m.group(1) if m else None
            if e["expected"] is None or e["expected"] != e["status"]:
                mismatches += 1
        worst = 1 if mismatches else 0

    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for e in manifest["shaders"]:
        print(f'{e["status"]:4} {e["source"]}: val_rc={e.get("spirv_val", {}).get("rc")} '
              f'refusal={len(e.get("ps5vk_driver_refusal", []))} gaps={len(e.get("ps5vk_feature_gaps", []))} '
              f'draw={len(e.get("ps5vk_draw_time_refusals", []))} expected={e.get("expected", "-")} '
              f'caps={",".join(e.get("capabilities", []))} exts={",".join(e.get("extensions", []))}')
    print(f"manifest: {a.out / 'manifest.json'}")
    return worst


if __name__ == "__main__":
    sys.exit(main())
