#!/usr/bin/env bash
# Xenos GPU (rexglue) host checks: prepare the patched sources, build the
# shader translator, translate the synthetic Xenos shaders
# (gpu/xenos/tools/synth_xenos.py, no game content) to SPIR-V and validate
# each module for Vulkan 1.0 with the standard block layout (what
# PS5_Vulkan accepts: no scalar block layout). Exit 0 iff all pass.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
b="$root/build"
"$root/gpu/xenos/rexglue/prepare.sh"
cmake -S "$root/gpu/xenos/rexglue" -B "$b/xenos-gpu-host" -G Ninja -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ -DRCOMP_ROOT="$root" >"$b/xenos-gpu-host.cfg.log"
ninja -C "$b/xenos-gpu-host" >"$b/xenos-gpu-host.build.log"
out="$b/xenos-gpu-spv"
rm -rf "$out" && mkdir -p "$out"
python3 "$root/gpu/xenos/tools/synth_xenos.py" "$out" >/dev/null
fails=0
for f in "$out"/*.xso; do
  n=$(basename "$f" .xso); t=ps; [[ $n == *vs_* ]] && t=vs
  if "$b/xenos-gpu-host/xenos_to_spirv" "$t" "$f" "$out/$n.spv" --container >/dev/null &&
     spirv-val --target-env vulkan1.0 "$out/$n.spv"; then
    echo "xenos-gpu/$n PASS"
  else
    echo "xenos-gpu/$n FAIL"; fails=$((fails + 1))
  fi
done
exit $(( fails ? 1 : 0 ))
