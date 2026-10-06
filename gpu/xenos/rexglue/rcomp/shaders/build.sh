#!/usr/bin/env bash
# Compiles R-comp's own Xenos helper shaders to SPIR-V C arrays (owner: gpu/xenos).
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
glslang=${GLSLANG_VALIDATOR:-glslangValidator}
for bpb in 2 3; do
  bits=$((8 << bpb))
  "$glslang" -V --target-env vulkan1.1 -DBPB_LOG2=$bpb \
    --vn texture_load_${bits}bpb_scaled_image_cs \
    -o texture_load_${bits}bpb_scaled_image_cs.h texture_load_scaled_image.comp
done
# Resolved depth textures: the 32-bit load with the guest depth converted to float32 (1: 24-bit unorm, 2: 20e4 float).
for mode in 1 2; do
  name=$([ "$mode" = 1 ] && echo unorm || echo float)
  "$glslang" -V --target-env vulkan1.1 -DBPB_LOG2=2 -DCONVERT_MODE=$mode \
    --vn texture_load_depth_${name}_scaled_image_cs \
    -o texture_load_depth_${name}_scaled_image_cs.h texture_load_scaled_image.comp
done
# The previous layout of the same load (a thread stores 8 consecutive blocks), kept behind the cvar rcomp_coalesced_load=false.
for bpb in 2 3; do
  bits=$((8 << bpb))
  "$glslang" -V --target-env vulkan1.1 -DBPB_LOG2=$bpb \
    --vn texture_load_${bits}bpb_scaled_image_v0_cs \
    -o texture_load_${bits}bpb_scaled_image_v0_cs.h texture_load_scaled_image_v0.comp
done
for mode in 1 2; do
  name=$([ "$mode" = 1 ] && echo unorm || echo float)
  "$glslang" -V --target-env vulkan1.1 -DBPB_LOG2=2 -DCONVERT_MODE=$mode \
    --vn texture_load_depth_${name}_scaled_image_v0_cs \
    -o texture_load_depth_${name}_scaled_image_v0_cs.h texture_load_scaled_image_v0.comp
done
