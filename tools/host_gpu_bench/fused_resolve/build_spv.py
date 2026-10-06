#!/usr/bin/env python3
"""Compiles the fused shader variants (fused_fast32.comp: the fast copy; fused_full.comp: the full copies), the fill shaders and the comparison shader of the host test into the SPIR-V
directory (fused_<variant>.spv, fill_<variant>.spv, compare.spv).
usage: build_spv.py [native2x 0|1] [depth_round 0|1] [scale 1..3] [out dir (default: spv next to this script)]
The dump shaders and the copy shaders of the SDK come from the same directory (extract_dump_spirv.py, make_dump_variants.py).
Environment: GLSLANG (the compiler), TARGET_ENV (vulkan1.1 default; vulkan1.0 as the title compiles), FUSED_FULL_COMP (another source for the full variants, to compare versions of the shader), FULL_EXTRA_DEFINES (e.g. "ROUNDTRIP_DIRECT=0")."""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
GLSLANG = os.environ.get("GLSLANG", "glslangValidator")  # e.g. build/vulkan-gta-radv-20260928/host-cygwin-full/bin/glslangValidator.exe
TARGET_ENV = os.environ.get("TARGET_ENV", "vulkan1.1")  # the title compiles for Vulkan 1.0 / SPIR-V 1.0 without optimization: TARGET_ENV=vulkan1.0 builds the same way
native2x = int(sys.argv[1]) if len(sys.argv) > 1 else 1
depth_round = int(sys.argv[2]) if len(sys.argv) > 2 else 0
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 3
out = Path(sys.argv[4]) if len(sys.argv) > 4 else HERE / "spv"
out.mkdir(exist_ok=True)

host2x = (1, 0) if native2x else (0, 3)

# name: (SRC_MSAA, SRC_IS_DEPTH, SRC_KIND, SRC_UINT, DEPTH_FORMAT, fill format, fill type, VALUE_KIND)
variants = {
    "c8888_1x": (0, 0, 1, 0, 0, "rgba8", "image2D", 0),
    "c8888_2x": (1, 0, 1, 0, 0, "rgba8", "image2DMS", 0),
    "c2101010_1x": (0, 0, 2, 0, 0, "rgba8", "image2D", 0),
    "c2101010_2x": (1, 0, 2, 0, 0, "rgba8", "image2DMS", 0),
    "c2101010f_1x": (0, 0, 3, 0, 0, "rgba16f", "image2D", 1),
    "c2101010f_2x": (1, 0, 3, 0, 0, "rgba16f", "image2DMS", 1),
    "c32f_1x": (0, 0, 4, 1, 0, "r32ui", "uimage2D", 2),
    "c32f_2x": (1, 0, 4, 1, 0, "r32ui", "uimage2DMS", 2),
    "c32fF_1x": (0, 0, 4, 0, 0, "r32ui", "uimage2D", 2),
    "d24s8_1x": (0, 1, 0, 0, 0, None, None, None),
    "d24fs8_1x": (0, 1, 0, 0, 1, None, None, None),
}

# The full copies (fused_full.comp), name: (SRC_MSAA, SRC_KIND, SRC_UINT, DEST_BPP, fill format, fill type, VALUE_KIND). Same names as kVariants of fused_resolve_bench.cpp.
FILL_8888 = ("rgba8", 0)
FILL_FP10 = ("rgba16f", 1)
FILL_32F = ("r32ui", 2)
full_variants = {}
for msaa_name, msaa in (("1x", 0), ("2x", 1), ("4x", 2)):
    image = "image2D" if msaa == 0 else "image2DMS"
    uimage = "uimage2D" if msaa == 0 else "uimage2DMS"
    for tag, kind, uint, fill in (("8888", 1, 0, FILL_8888), ("fp10", 3, 0, FILL_FP10), ("32f", 4, 1, FILL_32F)):
        for prefix, bpp in (("b32", 32), ("a64", 64)):
            full_variants[f"{prefix}_{tag}_{msaa_name}"] = (msaa, kind, uint, bpp, fill[0], uimage if fill[0] == "r32ui" else image, fill[1])
full_variants["b32_8888_2x_s01"] = full_variants["b32_8888_2x"]
full_variants["b32_8888_4x_sall"] = full_variants["b32_8888_4x"]
full_variants["a64_fp10_4x_sall"] = full_variants["a64_fp10_4x"]


def compile_glsl(src, dst, defines):
    cmd = [GLSLANG, "-V", "--target-env", TARGET_ENV] + [f"-D{k}={v}" for k, v in defines.items()] + ["-o", str(dst), str(src)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout, result.stderr)
        raise SystemExit(f"glslang failed for {dst.name}")


def host_samples(msaa):
    return 1 if msaa == 0 else (2 if native2x else 4) if msaa == 1 else 4


for name, (msaa, depth, kind, uint, depth_format, fill_format, fill_type, value_kind) in variants.items():
    compile_glsl(HERE / "fused_fast32.comp", out / f"fused_{name}.spv", {
        "SCALE_X": scale, "SCALE_Y": scale, "SRC_MSAA": msaa, "SRC_IS_DEPTH": depth, "SRC_KIND": kind, "SRC_UINT": uint,
        "DEPTH_FORMAT": depth_format, "DEPTH_ROUND": depth_round, "HOST_2X_SAMPLE_0": host2x[0], "HOST_2X_SAMPLE_1": host2x[1]})
    if fill_format:
        compile_glsl(HERE / "fill.comp", out / f"fill_{name}.spv", {"IMG_FORMAT": fill_format, "IMG_TYPE": fill_type, "SAMPLES": host_samples(msaa), "VALUE_KIND": value_kind})
for name, (msaa, kind, uint, bpp, fill_format, fill_type, value_kind) in full_variants.items():
    compile_glsl(Path(os.environ.get("FUSED_FULL_COMP", HERE / "fused_full.comp")), out / f"fused_{name}.spv", {
        "SCALE_X": scale, "SCALE_Y": scale, "SRC_MSAA": msaa, "SRC_KIND": kind, "SRC_UINT": uint, "DEST_BPP": bpp,
        "HOST_2X_SAMPLE_0": host2x[0], "HOST_2X_SAMPLE_1": host2x[1],
        **dict(item.split("=", 1) for item in os.environ.get("FULL_EXTRA_DEFINES", "").split() if "=" in item)})
    compile_glsl(HERE / "fill.comp", out / f"fill_{name}.spv", {"IMG_FORMAT": fill_format, "IMG_TYPE": fill_type, "SAMPLES": host_samples(msaa), "VALUE_KIND": value_kind})
    if value_kind == 2:  # random 32-bit floats: also without NaNs (HARNESS_NO_NAN=1)
        compile_glsl(HERE / "fill.comp", out / f"fill_{name}_nonan.spv", {"IMG_FORMAT": fill_format, "IMG_TYPE": fill_type, "SAMPLES": host_samples(msaa), "VALUE_KIND": value_kind, "NO_NAN": 1})
compile_glsl(HERE / "fill.comp", out / "fill_b32_32f_4x_nantag.spv", {"IMG_FORMAT": "r32ui", "IMG_TYPE": "uimage2DMS", "SAMPLES": 4, "VALUE_KIND": 2, "NAN_TAG": 1})  # NaN operand order experiment
compile_glsl(HERE / "compare.comp", out / "compare.spv", {})
print("built", len(variants), "fast and", len(full_variants), "full variants at scale", scale, "into", out)
