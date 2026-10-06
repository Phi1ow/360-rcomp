#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
base="$root/build/vulkan-gta-radv-20260928"
source_dir="$base/compiler-rt-reference"
sdk="$base/ps5-sdk"
build_dir="$base/compiler-rt-ps5"
export PATH=/usr/bin:/bin
export LLVM_CONFIG=/usr/bin/llvm-config
[[ $(git -C "$source_dir" rev-parse HEAD) == ca7933e47d3a3451d81e72ac174dcb5aa28b59d1 ]] || exit 2
cmake -S "$source_dir/compiler-rt/lib/builtins" -B "$build_dir" -G Ninja \
    -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER="$sdk/bin/prospero-clang" \
    -DCMAKE_CXX_COMPILER="$sdk/bin/prospero-clang++" \
    -DCMAKE_ASM_COMPILER="$sdk/bin/prospero-clang" \
    -DCMAKE_C_COMPILER_TARGET=x86_64-sie-ps5 \
    -DCMAKE_CXX_COMPILER_TARGET=x86_64-sie-ps5 \
    -DCMAKE_ASM_COMPILER_TARGET=x86_64-sie-ps5 \
    -DCMAKE_AR=/usr/bin/llvm-ar -DCMAKE_RANLIB=/usr/bin/llvm-ranlib \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_BUILD_TYPE=Release \
    -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BUILD_BUILTINS=ON \
    -DCOMPILER_RT_BUILTINS_ENABLE_PIC=ON -DLLVM_RUNTIMES_BUILD=ON \
    -DLLVM_MAIN_SRC_DIR="$source_dir/llvm" \
    -DCOMPILER_RT_INCLUDE_TESTS=OFF -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
    '-DCMAKE_C_FLAGS=-fno-stack-protector -fno-plt' \
    > "$base/compiler-rt-configure.log" 2>&1 || { tail -65 "$base/compiler-rt-configure.log"; exit 1; }
cmake --build "$build_dir" -j6 --target builtins > "$base/compiler-rt-build.log" 2>&1 ||
    { tail -75 "$base/compiler-rt-build.log"; exit 1; }
archive=$(find "$build_dir" -name 'libclang_rt.builtins*x86_64*.a' -print -quit)
test -n "$archive"
llvm-nm --defined-only "$archive" > "$base/compiler-rt-defined.txt"
grep -Eq ' [TW] __emutls_get_address$' "$base/compiler-rt-defined.txt"
sha256sum "$archive" > "$base/compiler-rt-archive.sha256"
cat "$base/compiler-rt-archive.sha256"
printf 'PASS native PS5 compiler-rt builtins and emulated TLS; console not yet tested\n'
