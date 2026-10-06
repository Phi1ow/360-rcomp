#!/bin/bash
# usage: run_all.sh <device index> <iterations> [seed]
# In the repository's Cygwin shell. Work directory ($WORK, default: the directory of this script) holds fused_resolve_bench.exe and spv/ (the dump shaders that the title logged,
# extract_dump_spirv.py, and the SDK's precompiled copy shaders). Runs the host test of every variant the fused resolve shaders support: the fast 32-bit copy (fused_fast32.comp) here,
# then the full copies (fused_full.comp, run_full.sh: scales 3, 2 and 1, the required and every destination format, bench). FULL=0 skips the full copies.
export PATH=/usr/bin:/bin
SRC=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$SRC}
cd "$WORK" || exit 2
python3 "$SRC/make_dump_variants.py" spv spv-s3 3 && python3 "$SRC/build_spv.py" 1 0 3 spv-s3 || exit 2
for v in c8888_1x c8888_2x c32f_1x d24fs8_1x c2101010f_1x; do
  timeout 3000 ./fused_resolve_bench.exe $1 3 $2 $v spv-s3 ${3:-1} 1 2>&1 | tail -12
done
if [ "${FULL:-1}" != "0" ]; then
  WORK=$WORK bash "$SRC/run_full.sh" "$1" "$2" "${3:-1}"
fi
echo "run_all done"
