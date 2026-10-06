#!/bin/bash
# usage: run_full.sh <device index> <iterations> [seed] [scales, default "3 2 1"]
# Host test of the fused full copies (fused_full.comp against resolve_full_32bpp_scaled_cs / resolve_full_64bpp_scaled_cs of the SDK) in the repository's Cygwin shell.
# Work directory ($WORK, default: the directory of this script) holds fused_resolve_bench.exe and spv/: the dump shaders that the title logged (extract_dump_spirv.py) and the SDK's
# precompiled copy shaders (resolve_full_32bpp_scaled_cs.spv, resolve_full_64bpp_scaled_cs.spv). For each scale it prepares spv-s<scale>/ (make_dump_variants.py: the dump shaders of the
# scale, the derived 32-bit float 4x dump shader; build_spv.py: the fused variants and the fill shaders), then runs every variant with the required destination format ("req") and with
# every destination format of the copy's class ("all"), and the bench of the required variants at scale 3. The 32-bit float variants run twice: with random bits as they come
# (NaNs included: the payload of a NaN that sums two NaNs differs, see the header of fused_full.comp) and without NaNs (HARNESS_NO_NAN=1).
export PATH=/usr/bin:/bin
SRC=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$SRC}
cd "$WORK" || exit 2
DEV=${1:-0}
ITER=${2:-300}
SEED=${3:-1}
SCALES=${4:-"3 2 1"}
REQUIRED="a64_fp10_1x a64_fp10_4x b32_8888_2x b32_32f_4x b32_fp10_1x"
EXTRA="b32_8888_1x b32_8888_4x b32_8888_2x_s01 b32_8888_4x_sall b32_fp10_4x a64_fp10_4x_sall b32_32f_1x a64_8888_1x a64_8888_2x a64_8888_4x a64_32f_1x"
unset HARNESS_NO_NAN HARNESS_TITLE_MASK HARNESS_FILL
for scale in $SCALES; do
  python3 "$SRC/make_dump_variants.py" spv spv-s$scale $scale $([ "$scale" = 3 ] && echo --check) || exit 2
  python3 "$SRC/build_spv.py" 1 0 $scale spv-s$scale || exit 2
  for v in $REQUIRED $EXTRA; do
    for mode in req all; do
      case $v in
        *32f*) nan_modes="0 1";;
        *) nan_modes="0";;
      esac
      for nonan in $nan_modes; do
        echo "== scale $scale, $v, destination formats: $mode$([ $nonan = 1 ] && echo ', no NaN contents')"
        HARNESS_NO_NAN=$nonan timeout 3000 ./fused_resolve_bench.exe $DEV $scale $ITER $v spv-s$scale $SEED 1 - $mode 2>&1 | grep -v "^  msaa" | tail -8
      done
    done
  done
done
echo "== bench (scale 3, 1280x256 px band, 640x256 for 4x), GPU timestamps: two passes (dump + SDK copy) against the fused shader"
for v in $REQUIRED; do
  timeout 3000 ./fused_resolve_bench.exe $DEV 3 200 $v spv-s3 $SEED 1 bench 2>&1 | tail -3
done
echo "run_full done"
