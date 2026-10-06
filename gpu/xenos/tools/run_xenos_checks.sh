#!/usr/bin/env bash
# R-comp Agent 5: reproduce the Xenos shader checks on a Linux host.
#
#   1. copy XenosRecomp @990d03b2 from $RCOMP_DEPS (read-only) into build/xenos-src,
#      fetch its submodules at the gitlink SHAs (dxc-bin, fmt, smol-v, xxHash, zstd),
#   2. build XenosRecomp twice: upstream (generic, no UNLEASHED_RECOMP) and
#      the PS5 profile (gpu/xenos/patches/0001-xenosrecomp-ps5-profile.patch,
#      -DXENOS_RECOMP_PS5),
#   3. emit SYNTHETIC containers (tools/synth_xenos.py; no game data), translate
#      them with both builds, compile with DXC using XenosRecomp's flags, and
#      gate the SPIR-V against PS5_Vulkan@9639c41 (tools/shader_manifest.py),
#   4. run the self-authored HLSL self-test with expectations.
#
# Real Xbox 360 shader corpus: BLOCKED (no authorized content) -- never fetched.
# Exit code: 0 iff every step behaves as expected (upstream output is EXPECTED
# to fail the PS5 gates; the PS5 profile and the self-test must match).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DEPS="${RCOMP_DEPS:-$ROOT/../deps}"
B="$ROOT/build"
XR_SHA=990d03b28a27b50277ee5d8d942e1c5f873869d1
TOOLS="$ROOT/gpu/xenos/tools"

declare -A SUB_URL=(
  [dxc-bin]=https://github.com/renderbag/dxc-bin.git
  [fmt]=https://github.com/fmtlib/fmt.git
  [smol-v]=https://github.com/aras-p/smol-v
  [xxHash]=https://github.com/Cyan4973/xxHash.git
  [zstd]=https://github.com/facebook/zstd.git
)

if [ ! -d "$B/xenos-src/.git" ]; then
  git clone -q "$DEPS/XenosRecomp" "$B/xenos-src"
fi
git -C "$B/xenos-src" checkout -q "$XR_SHA"
git -C "$B/xenos-src" checkout -q -- XenosRecomp
for n in "${!SUB_URL[@]}"; do
  sha=$(git -C "$B/xenos-src" ls-tree HEAD "thirdparty/$n" | awk '{print $3}')
  dir="$B/xenos-src/thirdparty/$n"
  if [ "$n" = dxc-bin ]; then dir="$B/xenos-deps/dxc-bin"; fi
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" != "$sha" ]; then
    rm -rf "$dir"; git init -q "$dir"
    git -C "$dir" remote add origin "${SUB_URL[$n]}"
    git -C "$dir" fetch -q --depth 1 origin "$sha"
    git -C "$dir" checkout -q FETCH_HEAD
  fi
  echo "submodule $n $(git -C "$dir" rev-parse HEAD)"
done
if [ ! -e "$B/xenos-src/thirdparty/dxc-bin/CMakeLists.txt" ]; then
  rm -rf "$B/xenos-src/thirdparty/dxc-bin"
  ln -s "$B/xenos-deps/dxc-bin" "$B/xenos-src/thirdparty/dxc-bin"
fi
DXC_DIR="$B/xenos-deps/dxc-bin"
chmod +x "$DXC_DIR/bin/x64/dxc-linux"

build() { # $1 build dir, $2 extra CXX flags
  cmake -S "$B/xenos-src" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=$2" >"$1.configure.log" 2>&1
  cmake --build "$1" --target XenosRecomp >"$1.build.log" 2>&1
}
build "$B/xenos-host" ""
git -C "$B/xenos-src" apply "$ROOT/gpu/xenos/patches/0001-xenosrecomp-ps5-profile.patch"
build "$B/xenos-host-ps5" "-DXENOS_RECOMP_PS5"
git -C "$B/xenos-src" checkout -q -- XenosRecomp   # leave the copy pristine

S="$B/xenos-synth"; rm -rf "$S"; mkdir -p "$S/hlsl" "$S/hlsl-ps5"
python3 "$TOOLS/synth_xenos.py" "$S/in"
for f in "$S"/in/*.xso; do
  b=$(basename "$f" .xso)
  "$B/xenos-host/XenosRecomp/XenosRecomp" "$f" "$S/hlsl/$b.hlsl" "$B/xenos-src/XenosRecomp/shader_common.h"
  "$B/xenos-host-ps5/XenosRecomp/XenosRecomp" "$f" "$S/hlsl-ps5/$b.hlsl" "$ROOT/gpu/xenos/prelude/shader_common_ps5.h"
done

M=(python3 "$TOOLS/shader_manifest.py" --dxc "$DXC_DIR/bin/x64/dxc-linux" --dxc-lib "$DXC_DIR/lib/x64")
set +e
"${M[@]}" --corpus "$S/hlsl" --out "$S/spv-upstream" --corpus-kind synthetic-xenosrecomp-upstream
up=$?
"${M[@]}" --corpus "$S/hlsl-ps5" --out "$S/spv-ps5" --corpus-kind synthetic-xenosrecomp-ps5-profile
ps5=$?
"${M[@]}" --corpus "$TOOLS/selftest" --out "$B/xenos-selftest" --check-expectations
st=$?
set -e
echo "upstream-profile gate rc=$up (expected 1: BDA/Int64/bindless)"
echo "ps5-profile gate rc=$ps5 (expected 0)"
echo "selftest expectations rc=$st (expected 0)"
echo "real Xenos corpus: BLOCKED (no authorized content)"
[ "$up" = 1 ] && [ "$ps5" = 0 ] && [ "$st" = 0 ]
