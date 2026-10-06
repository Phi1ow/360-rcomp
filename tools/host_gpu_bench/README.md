# Host GPU bench of the resolution-scaled texture load shaders

Not PS5 evidence: it runs on a Vulkan device of the Windows host (the machine's AMD GPUs, through `vulkan-1.dll`), to prove that a rewrite of
`gpu/xenos/rexglue/rcomp/shaders/texture_load_scaled_image.comp` produces the same image bit for bit as the shader it replaces, and to compare the
time of the two. `load_bench.cpp` loads the Vulkan loader dynamically (no import library), fills a pseudo-random "scaled resolve buffer" of the
guest layout, runs a reference shader and a variant with the push constants of `VulkanTextureCache::LoadConstants` (including band loads with a guest
offset and a host row offset), compares the whole destination images (zero-filled first), then times both with timestamp queries.

- `texture_load_scaled_image_v0.comp`: the shader of phases 54-112 (a thread stores 8 consecutive blocks).
- `texture_load_scaled_image_v1.comp`: coalesced stores (adopted from phase 118 on; the repository shader is this one plus its documentation).
- Build (the repository's Cygwin shell, the Vulkan headers of the private Xenos copy):
  `g++ -std=gnu++20 -O2 -I<xenos-source-v3>/thirdparty/vulkan-headers/include load_bench.cpp -o load_bench.exe`
- Shaders: `glslangValidator -V --target-env vulkan1.1 -DBPB_LOG2=2|3 [-DCONVERT_MODE=1|2] -o X.spv X.comp`.
- Run: `load_bench.exe <device> <width> <height> <scale> <bpb_log2> <convert> <reference.spv> <variant.spv> <variant blocks per group x> <variant rows per group> <iterations>`,
  and `HARNESS_BAND=<first guest row>,<rows>` for a band load. Device 0 and 1 are the order of `vulkaninfo --summary`.
- Result of phase 118's change: `docs/evidence/wave5-20261001/load_shader_bench.txt` (25 configurations, all bit-identical, 1.02-1.55x faster).

## Resolve copy shader transcription and the fused dump + resolve copy (wave 6)

- `resolve_bench.cpp`: bit-exactness of a GLSL transcription (`copy_clone.comp`, from the disassembly of the SDK's `resolve_fast_32bpp_1x2xmsaa_scaled_cs`, whose
  source is not in the repository) against the precompiled copy shader, 300 random parameter sets on each of two GPUs, 0 differences (511 million words).
- `fused_resolve/`: the host test of the fused render target dump + resolve copy (`rcomp_fused_resolve`): the title's own dump shader (its SPIR-V is logged by a run with
  `-DRCOMP_M6_XENOS_CVARS=rcomp_diag_log_dump_spirv=true`, `extract_dump_spirv.py <rcomp_title.err> spv`), the original copy shader (`spv/copy.spv`) and the fused shader
  (`fused_fast32.comp`, compiled by `build_spv.py` with the variant's `#define`s) run on a random render target image and random legal parameters, the destinations compared on the GPU.
  Build: `g++ -std=gnu++20 -O2 -I<xenos-source-v3>/thirdparty/vulkan-headers/include fused_resolve_bench.cpp -o fused_resolve_bench.exe` (the repository's Cygwin shell);
  run: `run_all.sh <device> <iterations> [seed]`, or `fused_resolve_bench.exe <device> <scale> <iterations> <variant> spv [seed] [native2x] [bench]`. The results are in
  `docs/evidence/wave6-20261001/fused_resolve_host_test.txt`. The same GLSL, with the variant's `#define`s written in front of it, is what the title compiles at run time.
