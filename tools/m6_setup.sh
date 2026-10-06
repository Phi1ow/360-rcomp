#!/usr/bin/env bash
# Builds only the host tools tools/m6_inventory.py needs (Linux, WSL, macOS, or
# on Windows the Cygwin of tools/bootstrap_cygwin.py: python tools/setup_windows.py
# runs it there): patched XenonRecomp + XenonAnalyse, and rcomp_xex_decode. No PS5
# SDK, no reference checkouts, no game file involved. The tools land in
# build/cpu-xenonrecomp and build/cpu-xex-decode; the installer finds them there.
#   tools/m6_setup.sh        (needs git, cmake, ninja, clang/clang++)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
b="$root/build"
mkdir -p "$b"
git -C "$root" submodule update --init --recursive third_party/XenonRecomp
"$root/cpu/tools/prepare_xenonrecomp.sh"
cmake -S "$b/cpu-xenonrecomp-src" -B "$b/cpu-xenonrecomp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >"$b/cpu-xenonrecomp.cfg.log"
ninja -C "$b/cpu-xenonrecomp" XenonRecomp XenonAnalyse >"$b/cpu-xenonrecomp.build.log"
x="$b/cpu-xenonrecomp-src"
clang++ -std=c++20 -O2 -I "$x/XenonUtils" -I "$x/thirdparty/simde" -I "$x/thirdparty/tiny-AES-c" \
    -I "$x/thirdparty/fmt/include" "$root/cpu/tools/xex_decode.cpp" \
    "$b/cpu-xenonrecomp/XenonUtils/libXenonUtils.a" "$b/cpu-xenonrecomp/thirdparty/disasm/libdisasm.a" \
    "$b/cpu-xenonrecomp/thirdparty/fmt/libfmt.a" -o "$b/cpu-xex-decode"
echo "ready: tools/m6_build_title.sh /outside/repo/game.xex ART SDK [name]"
