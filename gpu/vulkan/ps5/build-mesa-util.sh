#!/usr/bin/env bash
# R-comp - Agent 4 (PS5_Vulkan integration).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The three Mesa utility objects a *static* title link of PS5_Vulkan needs and
# PS5_Vulkan's archives do not carry. Same inputs and flags as
# PS5_RetroArch@18dc105 tools/build-mesa-util.sh (see LINK_CONTRACT.md §4):
#
#   src/util/u_thread.c  u_thread_create u_thread_setname util_barrier_init
#                        util_barrier_destroy util_barrier_wait
#                        util_thread_get_time_nano
#   src/util/anon_file.c os_create_anonymous_file   (without -D_XOPEN_SOURCE=700)
#   src/util/os_file.c   os_read_file               (with -D__ORBIS__)
#
# compiled from the PS5_Vulkan build root's compiler work copy
# (.deps/work/psbc-ps5/third_party/opengnm-psbc), i.e. the same tree whose
# u_queue.ps5.o is inside libpsbc_driver.ps5.a.
#
#   ps5/build-mesa-util.sh DRIVER_ROOT SDK_ROOT [OUT_DIR]
#
# DRIVER_ROOT: a PS5_Vulkan build root (a copy, never /home/user/deps).
# OUT_DIR defaults to DRIVER_ROOT/build/rcomp-mesa-util. Prints object paths.
set -euo pipefail

driver=$(cd -- "${1:?DRIVER_ROOT}" && pwd)
sdk=$(cd -- "${2:?SDK_ROOT}" && pwd)
out=${3:-$driver/build/rcomp-mesa-util}
case $driver in /home/user/deps/*) echo "error: refusing to build inside $driver" >&2; exit 2 ;; esac
third_party="$driver/.deps/work/psbc-ps5/third_party"
mesa="$third_party/opengnm-psbc"
wrapper="$driver/tooling/prospero-clang18"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}

sources=(src/util/u_thread.c src/util/anon_file.c src/util/os_file.c)
symbols_for() {
    case ${1##*/} in
        u_thread.c) printf '%s\n' u_thread_create u_thread_setname util_barrier_init \
            util_barrier_destroy util_barrier_wait util_thread_get_time_nano ;;
        anon_file.c) printf '%s\n' os_create_anonymous_file ;;
        os_file.c) printf '%s\n' os_read_file ;;
    esac
}
for f in "${sources[@]/#/$mesa/}" "$third_party/opengnm/include" \
         "$third_party/Vulkan-Headers/include" "$wrapper" "$sdk/bin/llvm-nm"; do
    [[ -e $f ]] || { echo "BLOCKED: missing $f (build the driver first)" >&2; exit 2; }
done

shared_flags=(
    -Iinclude/ -Ilibpsbc/ -I"$third_party/opengnm/include" -I"$third_party/Vulkan-Headers/include"
    -Isrc/ -Isrc/amd -Isrc/amd/common -Isrc/amd/common/nir -Isrc/amd/compiler -Isrc/amd/vulkan
    -Isrc/amd/vulkan/nir -Isrc/vulkan/runtime -Isrc/vulkan/runtime/bvh -Isrc/vulkan/util
    -Isrc/compiler -Isrc/compiler/nir -Isrc/compiler/spirv -Isrc/gallium/include -Isrc/mesa
    -Isrc/mesa/main -Isrc/util -Icmd/psbc -Iinclude/mesa
    -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0
    -DHAVE_STRUCT_TIMESPEC=1 -DHAVE_PTHREAD=1 -DHAVE_PTHREAD_NP_H=1 -DHAVE_SYSCONF=1
    -DHAVE_FUNC_ATTRIBUTE_PACKED=1 -Dalloca=__builtin_alloca -include strings.h
    -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512
)
mkdir -p "$out"
for source in "${sources[@]}"; do
    object="$out/$(basename "${source%.c}").o"
    flags=("${shared_flags[@]}")
    case ${source##*/} in
        anon_file.c) # SHM_ANON needs __BSD_VISIBLE, which _XOPEN_SOURCE=700 clears.
            kept=(); for f in "${flags[@]}"; do [[ $f == -D_XOPEN_SOURCE=700 ]] || kept+=("$f"); done
            flags=("${kept[@]}") ;;
        os_file.c) flags+=(-D__ORBIS__) ;; # skips the kernel-only KERN_FILE branch
    esac
    (cd "$mesa" && PS5_PAYLOAD_SDK="$sdk" sh "$wrapper" \
        -std=gnu11 -O2 -g -Wall -fPIC -DOPENGNM_PSBC_ORBIS=1 -Dstatic_assert=_Static_assert \
        -Wno-unused-function -Wno-unused-variable -Wno-unreachable-code-generic-assoc \
        "${flags[@]}" -c "$source" -o "$object") 2>&1 | sed "s|^|[$source] |" >&2
    [[ -f $object ]] || { echo "error: $source did not compile" >&2; exit 1; }
    while read -r symbol; do
        "$sdk/bin/llvm-nm" --defined-only "$object" | grep -qE " [TtWw] $symbol\$" ||
            { echo "error: $object does not define $symbol" >&2; exit 1; }
    done < <(symbols_for "$source")
    echo "$object"
done
