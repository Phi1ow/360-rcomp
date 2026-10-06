#!/usr/bin/env bash
# R-comp PS5 test title build (owner: Agent 2).
#
#   platform/ps5/tools/build_title.sh              build the title folder
#   platform/ps5/tools/build_title.sh --package    also a .ffpkg/.ffpfsc image (BLOCKED)
#   platform/ps5/tools/build_title.sh --cpu-corpus also run the tests/cpu corpus
#                                                  (same as RCOMP_TITLE_CPU_CORPUS=1)
#   platform/ps5/tools/build_title.sh --m3         also run the M3 program (tests/m3)
#   platform/ps5/tools/build_title.sh --xex        also load and run the synthetic XEX
#                                                  (tests/xex, build/xex/rcomp_title.xex)
#
# --cpu-corpus links the archives of an existing PS5 corpus build (this script
# does not write outside build/platform-*):
#   $RCOMP_CPU_CORPUS_BUILD (default build/tests-cpu-ps5)/librcomp_cpu_selftest.a
#   $RCOMP_CPU_CORPUS_BUILD/librcomp_cpu_generated.a
# and outputs to build/platform-ps5-title-cpu/ instead of build/platform-ps5-title/.
# The platform archive always comes from step 1 (build/platform-ps5), never
# from the corpus build's copy, so exactly one rcomp_platform is linked.
#
# Output: build/platform-ps5-title/dist/<TITLE_ID>/{eboot.bin,sce_sys/,sce_module/libc.prx}
# plus manifest.sha256. Nothing is deployed and nothing touches a console.
#
# Pipeline (see platform/ps5/README.md "How a title is built"):
#   1. cmake + cmake/toolchains/ps5.cmake -> librcomp_platform.a, librcomp_platform_selftest.a
#   2. prospero-clang++ -c  our CRT (title/crt) and title/main.cpp
#   3. prospero-lld with title/crt/rcomp_title.ld -> intermediate PIE (R-X/R/RELRO/RW)
#   4. ps5-native-tool link  -> PS5 dynamic ELF (imports bound by NID to SDK stubs)
#   5. ps5-native-tool self --sign -> fake-signed eboot.bin
#   6. libc.prx (clean-room runtime module required in sce_module/) + sce_sys/
#
# Steps 4-6 use host tools that are only available as GPL-3.0 source in the
# pinned PS5_RetroArch checkout (tooling/native, from BlackBearReloaded's
# ps5-native-app-boilerplate). They are compiled into build/, never vendored.
# Using them is a licensing decision for PRIME, so it is opt-in:
#   RCOMP_ACCEPT_GPL_TITLE_TOOLS=1
# Without it the script stops after step 3 with a BLOCKED message (exit 3).
#
# Environment:
#   RCOMP_DEPS        reference checkouts (default: <repo>/../deps)
#   RCOMP_PS5_SDK     payload SDK (default: $PS5_PAYLOAD_SDK or $RCOMP_DEPS/sdk/ps5-payload-sdk)
#   RCOMP_XEX_PACKAGED_BOOT=1  --xex uses the packaged production TitleRuntime boot test
#   RCOMP_XEX_BOOT_DATA_FILE   original four-byte boot-data.bin required in that mode
#   RCOMP_XEX_POOL_RTL=1      --xex uses the original pool/Rtl test (XEX only)
#   RCOMP_XEX_MODULES=1       --xex uses the original modules test (XEX only)
#   RCOMP_XEX_CALENDAR_THREADS=1 --xex uses the original calendar/threads test (XEX only)
#   RCOMP_XEX_SERVICES=1      --xex uses the original services test + boot-data.bin
#   RCOMP_PS5_MODULE_SDK / RCOMP_PS5_COMPANION_SDK / RCOMP_PS5_FSELF_MAGIC
#                     loader constants (defaults are the values PS5_RetroArch
#                     records as validated on firmware 6.02 and 12.70)
# Exit: 0 built, 1 build step failed, 2 usage/prerequisite, 3 BLOCKED.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
here="$root/platform/ps5"
deps=${RCOMP_DEPS:-$(cd -- "$root/.." && pwd)/deps}
sdk=${RCOMP_PS5_SDK:-${PS5_PAYLOAD_SDK:-$deps/sdk/ps5-payload-sdk}}
out="$root/build/platform-ps5-title"
lib_build="$root/build/platform-ps5"
tools_build="$root/build/platform-ps5-tools"
ra="$deps/PS5_RetroArch"
ra_pin=18dc1059c497c172d7bfa3c0ab28de7e3e45bf39
libc_raw_sha=8ee6e124993e1af26420cb455890fd002f5d6c7e78883c860ce45734e7d002bb
libc_prx_sha=e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036
module_sdk=${RCOMP_PS5_MODULE_SDK:-0x02000009}
companion_sdk=${RCOMP_PS5_COMPANION_SDK:-0x08050001}
fself_magic=${RCOMP_PS5_FSELF_MAGIC:-0x1D3D154F}

package=0
cpu_corpus=${RCOMP_TITLE_CPU_CORPUS:-0}
m3=${RCOMP_TITLE_M3:-0}
xex=${RCOMP_TITLE_XEX:-0}
xex_packaged_boot=${RCOMP_XEX_PACKAGED_BOOT:-0}
xex_pool_rtl=${RCOMP_XEX_POOL_RTL:-0}
xex_modules=${RCOMP_XEX_MODULES:-0}
xex_calendar_threads=${RCOMP_XEX_CALENDAR_THREADS:-0}
xex_services=${RCOMP_XEX_SERVICES:-0}
xex_kernel_boot=${RCOMP_XEX_KERNEL_BOOT:-0}
xex_boot_rehearsal=${RCOMP_XEX_BOOT_REHEARSAL:-0}
xenos_cp=${RCOMP_TITLE_XENOS_CP:-0}
for arg in "$@"; do
    case "$arg" in
        --package) package=1 ;;
        --cpu-corpus) cpu_corpus=1 ;;
        --m3) m3=1 ;;
        --xex) xex=1 ;;
        --xenos-cp) xenos_cp=1 ;;
        *) echo "usage: ${0##*/} [--package] [--cpu-corpus] [--m3] [--xex] [--xenos-cp]" >&2; exit 2 ;;
    esac
done
[[ $cpu_corpus == 0 || $cpu_corpus == 1 ]] ||
    { echo "RCOMP_TITLE_CPU_CORPUS must be 0 or 1" >&2; exit 2; }
[[ $m3 == 0 || $m3 == 1 ]] ||
    { echo "RCOMP_TITLE_M3 must be 0 or 1" >&2; exit 2; }
[[ $xex == 0 || $xex == 1 ]] ||
    { echo "RCOMP_TITLE_XEX must be 0 or 1" >&2; exit 2; }
[[ $xenos_cp == 0 || $xenos_cp == 1 ]] ||
    { echo "RCOMP_TITLE_XENOS_CP must be 0 or 1" >&2; exit 2; }
[[ $xex_packaged_boot == 0 || $xex_packaged_boot == 1 ]] ||
    { echo "RCOMP_XEX_PACKAGED_BOOT must be 0 or 1" >&2; exit 2; }
[[ $xex_pool_rtl == 0 || $xex_pool_rtl == 1 ]] ||
    { echo "RCOMP_XEX_POOL_RTL must be 0 or 1" >&2; exit 2; }
[[ $xex_modules == 0 || $xex_modules == 1 ]] ||
    { echo "RCOMP_XEX_MODULES must be 0 or 1" >&2; exit 2; }
[[ $xex_calendar_threads == 0 || $xex_calendar_threads == 1 ]] ||
    { echo "RCOMP_XEX_CALENDAR_THREADS must be 0 or 1" >&2; exit 2; }
[[ $xex_services == 0 || $xex_services == 1 ]] ||
    { echo "RCOMP_XEX_SERVICES must be 0 or 1" >&2; exit 2; }
[[ $xex_kernel_boot == 0 || $xex_kernel_boot == 1 ]] ||
    { echo "RCOMP_XEX_KERNEL_BOOT must be 0 or 1" >&2; exit 2; }
[[ $xex_boot_rehearsal == 0 || $xex_boot_rehearsal == 1 ]] ||
    { echo "RCOMP_XEX_BOOT_REHEARSAL must be 0 or 1" >&2; exit 2; }
if (( xex_packaged_boot && !xex )); then
    echo "RCOMP_XEX_PACKAGED_BOOT=1 requires --xex" >&2; exit 2
fi
if (( xex_pool_rtl && !xex )); then
    echo "RCOMP_XEX_POOL_RTL=1 requires --xex" >&2; exit 2
fi
if (( xex_modules && !xex )); then
    echo "RCOMP_XEX_MODULES=1 requires --xex" >&2; exit 2
fi
if (( xex_calendar_threads && !xex )); then
    echo "RCOMP_XEX_CALENDAR_THREADS=1 requires --xex" >&2; exit 2
fi
if (( xex_services && !xex )); then
    echo "RCOMP_XEX_SERVICES=1 requires --xex" >&2; exit 2
fi
if (( xex_kernel_boot && !xex )); then
    echo "RCOMP_XEX_KERNEL_BOOT=1 requires --xex" >&2; exit 2
fi
if (( xex_packaged_boot + xex_pool_rtl + xex_modules + xex_calendar_threads + xex_services + xex_kernel_boot + xex_boot_rehearsal > 1 )); then
    echo "RCOMP_XEX_PACKAGED_BOOT, RCOMP_XEX_POOL_RTL, RCOMP_XEX_MODULES, RCOMP_XEX_CALENDAR_THREADS, RCOMP_XEX_SERVICES, RCOMP_XEX_KERNEL_BOOT and RCOMP_XEX_BOOT_REHEARSAL are mutually exclusive" >&2; exit 2
fi
if (( xex_boot_rehearsal )); then
    echo "RCOMP_XEX_BOOT_REHEARSAL is host-only and cannot build a PS5 title" >&2; exit 2
fi
corpus_build=${RCOMP_CPU_CORPUS_BUILD:-$root/build/tests-cpu-ps5}
corpus_archives=()
title_defs=()
if (( cpu_corpus )); then
    out="$root/build/platform-ps5-title-cpu"
    corpus_archives=("$corpus_build/librcomp_cpu_selftest.a" "$corpus_build/librcomp_cpu_generated.a")
    for a in "${corpus_archives[@]}"; do
        if [[ ! -f $a ]]; then
            echo "missing $a; build the PS5 corpus first (tests/ owns it):" >&2
            echo "  cmake -S tests/cpu -B build/tests-cpu-ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=\$PWD/cmake/toolchains/ps5.cmake -DRCOMP_ROOT=\$PWD -DCORPUS_DIR=\$PWD/build/cpu-corpus -DCASES_DIR=\$PWD/build/tests-cpu-cases && ninja -C build/tests-cpu-ps5" >&2
            exit 2
        fi
    done
    title_defs=(-DRCOMP_TITLE_CPU_CORPUS)
fi
# --m3: the M3 guest program (tests/m3) on the guest runtime (runtime/), which
# needs the C++ standard library: libc++/libc++abi/libunwind are linked
# statically from the payload SDK. The fixture data file is packaged under
# fixtures/m3/data/ in the title folder.
m3_build=${RCOMP_M3_BUILD:-$root/build/tests-m3-ps5}
m3_archives=()
if (( m3 )); then
    out="${out%-cpu}-m3"
    (( cpu_corpus )) && out="$root/build/platform-ps5-title-cpu-m3"
    m3_archives=("$m3_build/librcomp_m3_selftest.a" "$m3_build/librcomp_m3_generated.a"
                 "$m3_build/runtime/librcomp_runtime.a"
                 "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a")
    for a in "${m3_archives[@]}"; do
        [[ -f $a ]] || { echo "missing $a; build tests/m3 for PS5 first (build/tests-m3-ps5)" >&2; exit 2; }
    done
    title_defs+=(-DRCOMP_TITLE_M3)
fi

# --xex: XenonRecomp XEX-mode output of the synthetic title (tests/xex) on the
# guest runtime; the .xex itself is packaged under fixtures/xex/ and loaded
# at run time by the runtime's XEX loader.
xex_build=${RCOMP_XEX_BUILD:-$root/build/tests-xex-ps5}
xex_file=${RCOMP_XEX_FILE:-$root/build/xex/rcomp_title.xex}
xex_boot_data=${RCOMP_XEX_BOOT_DATA_FILE:-}
xex_archives=()
xex_title_runtime_archives=()
xex_shared=()
if (( xex )); then
    (( m3 || cpu_corpus )) && { echo "--xex cannot be combined with --m3/--cpu-corpus (one guest runtime per title)" >&2; exit 2; }
    out="$root/build/platform-ps5-title-xex"
    xex_archives=("$xex_build/librcomp_xex_selftest.a" "$xex_build/librcomp_xex_generated.a"
                  "$xex_build/runtime/librcomp_runtime.a"
                  "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a")
    if (( xex_packaged_boot )); then
        out="$root/build/platform-ps5-title-boot"
    fi
    if (( xex_services )); then
        out="$root/build/platform-ps5-title-services"
    fi
    if (( xex_kernel_boot )); then
        out="$root/build/platform-ps5-title-kernel-boot"
    fi
    if (( xex_packaged_boot || xex_services )); then
        [[ -n $xex_boot_data && -f $xex_boot_data ]] ||
            { echo "RCOMP_XEX_BOOT_DATA_FILE must name the original boot-data.bin" >&2; exit 2; }
        python3 - "$xex_boot_data" <<'PY' || exit 2
import pathlib, sys
if pathlib.Path(sys.argv[1]).read_bytes() != bytes((1, 2, 3, 4)):
    raise SystemExit("boot-data.bin must contain exactly 01 02 03 04")
PY
    fi
    if (( xex_pool_rtl )); then
        out="$root/build/platform-ps5-title-pool-rtl"
    fi
    if (( xex_modules )); then
        out="$root/build/platform-ps5-title-modules"
    fi
    if (( xex_calendar_threads )); then
        out="$root/build/platform-ps5-title-calendar-threads"
    fi
    if (( xex_packaged_boot || xex_pool_rtl || xex_modules || xex_calendar_threads || xex_services || xex_kernel_boot )); then
        xex_title_runtime_archives=("$xex_build/runtime/librcomp_runtime_video.a"
                                    "$xex_build/platform/librcomp_platform_input.a")
        xex_shared=("$sdk/target/lib/libScePad.so" "$sdk/target/lib/libSceUserService.so")
        xex_archives+=("${xex_title_runtime_archives[@]}")
    fi
    for a in "${xex_archives[@]}" "${xex_shared[@]}" "$xex_file"; do
        [[ -f $a ]] || { echo "missing $a; run tools/cpu_pipeline.sh and build tests/xex for PS5 (build/tests-xex-ps5)" >&2; exit 2; }
    done
    title_defs+=(-DRCOMP_TITLE_XEX)
fi

# --xenos-cp: the Xenos PM4 command processor (rexglue, gpu/xenos/rexglue) over
# guest memory with a recording TESTDOUBLE backend (tests/xenos_cp).
xcp_build=${RCOMP_XENOS_CP_BUILD:-$root/build/tests-xenos-cp-ps5}
xcp_archives=()
if (( xenos_cp )); then
    (( m3 || cpu_corpus || xex )) && { echo "--xenos-cp cannot be combined with other modes" >&2; exit 2; }
    out="$root/build/platform-ps5-title-xenos-cp"
    xcp_archives=("$xcp_build/librcomp_xenos_cp_selftest.a" "$xcp_build/librcomp_xenos_video_selftest.a"
                  "$xcp_build/runtime/librcomp_runtime_video.a" "$xcp_build/runtime/librcomp_runtime.a"
                  "$xcp_build/librcomp_xenos_video_functable.a" "$xcp_build/xenos/librcomp_xenos_host.a"
                  "$xcp_build/xenos/librcomp_xenos_cp.a" "$xcp_build/xenos/librcomp_xenos_shader.a"
                  "$xcp_build/xenos/librcomp_rex_base.a" "$xcp_build/xenos/librcomp_glslang_spirv.a"
                  "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a")
    for a in "${xcp_archives[@]}"; do
        [[ -f $a ]] || { echo "missing $a; build tests/xenos_cp for PS5 first (build/tests-xenos-cp-ps5)" >&2; exit 2; }
    done
    title_defs+=(-DRCOMP_TITLE_XENOS_CP)
fi

# The production runtime now owns native NetDll cancellation, so every title
# containing it must bind its PS5 backend to the actual SceNet module.
runtime_shared=()
if (( m3 || xex || xenos_cp )); then
    runtime_shared=("$sdk/target/lib/libSceNet.so")
    [[ -f ${runtime_shared[0]} ]] || { echo "missing ${runtime_shared[0]}" >&2; exit 2; }
fi

say() { printf '==> [title] %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
blocked() { printf 'BLOCKED: %s\n' "$*" >&2; exit 3; }
no_wx_load_segments() {
    local elf=$1
    llvm-readelf --program-headers "$elf" | awk '
        /^[[:space:]]*LOAD[[:space:]]/ {
            writable = executable = 0
            for (i = 7; i < NF; ++i) {
                if ($i ~ /W/) writable = 1
                if ($i ~ /E/) executable = 1
            }
            if (writable && executable) bad = 1
        }
        END { exit bad }
    '
}

for c in cmake ninja python3 sha256sum clang++; do
    command -v "$c" >/dev/null || { echo "missing required command: $c" >&2; exit 2; }
done
[[ -x $sdk/bin/prospero-clang++ && -x $sdk/bin/prospero-lld ]] ||
    { echo "no ps5-payload-sdk at $sdk (set RCOMP_PS5_SDK or RCOMP_DEPS)" >&2; exit 2; }

param="$here/title/sce_sys/param.json"
title_id=$(python3 - "$param" <<'PY'
import json, re, sys
v = json.load(open(sys.argv[1], encoding="utf-8"))
t = v.get("titleId", "")
if not re.fullmatch(r"PPSA\d{5}", t):
    raise SystemExit("titleId must be PPSA + 5 digits")
if not re.fullmatch(r"[A-Z]{2}\d{4}-PPSA\d{5}_00-[A-Z0-9]{16}", v.get("contentId", "")) \
        or t not in v["contentId"]:
    raise SystemExit("contentId must be XXdddd-<titleId>_00-<16 [A-Z0-9]>")
if not re.fullmatch(r"\d{5}", v.get("conceptId", "")):
    raise SystemExit("conceptId must be 5 digits")
print(t)
PY
) || { echo "invalid $param" >&2; exit 2; }

mkdir -p "$out/obj"
build_id=$(cat "$here/title/main.cpp" "$here/title/crt/"* "$here/os_vm_ps5.cpp" \
    "$root/platform/common/"*.cpp "$root/platform/tests/test_guest_memory.cpp" \
    "$root/include/rcomp/guest_memory.h" | sha256sum | cut -c1-16)
if (( xex )); then
    build_id=$( (printf '%s\n' "$build_id"; cat "${xex_archives[@]:0:3}" "$xex_file") | sha256sum | cut -c1-16)
    if (( xex_packaged_boot )); then
        build_id=$( (printf '%s\npackaged-boot\n' "$build_id";
            cat "${xex_title_runtime_archives[@]}" "${xex_shared[@]}" "$xex_boot_data") | sha256sum | cut -c1-16)
    elif (( xex_pool_rtl )); then
        build_id=$( (printf '%s\npool-rtl\n' "$build_id";
            cat "${xex_title_runtime_archives[@]}" "${xex_shared[@]}") | sha256sum | cut -c1-16)
    elif (( xex_modules )); then
        build_id=$( (printf '%s\nmodules\n' "$build_id";
            cat "${xex_title_runtime_archives[@]}" "${xex_shared[@]}") | sha256sum | cut -c1-16)
    elif (( xex_calendar_threads )); then
        build_id=$( (printf '%s\ncalendar-threads\n' "$build_id";
            cat "${xex_title_runtime_archives[@]}" "${xex_shared[@]}") | sha256sum | cut -c1-16)
    elif (( xex_services )); then
        build_id=$( (printf '%s\nservices\n' "$build_id";
            cat "${xex_title_runtime_archives[@]}" "${xex_shared[@]}" "$xex_boot_data") | sha256sum | cut -c1-16)
    elif (( xex_kernel_boot )); then
        build_id=$( (printf '%s\nkernel-boot\n' "$build_id";
            cat "${xex_title_runtime_archives[@]}" "${xex_shared[@]}") | sha256sum | cut -c1-16)
    fi
fi
if (( ${#runtime_shared[@]} )); then
    build_id=$( (printf '%s\n' "$build_id"; cat "${runtime_shared[@]}") | sha256sum | cut -c1-16)
fi
if (( xenos_cp )); then
    build_id=$( (printf '%s\n' "$build_id"; cat "${xcp_archives[@]:0:6}") | sha256sum | cut -c1-16)
fi
if (( cpu_corpus )); then
    build_id=$( (printf '%s\n' "$build_id"; cat "${corpus_archives[@]}") | sha256sum | cut -c1-16)
    say "cpu corpus mode: linking ${corpus_archives[*]}"
fi
say "title $title_id, source digest $build_id"

# --- 1. platform libraries ---------------------------------------------------
say "step 1/6: platform libraries (cmake, PS5 toolchain)"
cmake -S "$root/platform" -B "$lib_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/ps5.cmake" \
    -DRCOMP_PS5_SDK="$sdk" -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$out/cmake-configure.log" 2>&1 ||
    { tail -20 "$out/cmake-configure.log"; die "cmake configure failed"; }
cmake --build "$lib_build" --target rcomp_platform rcomp_platform_selftest \
    >"$out/cmake-build.log" 2>&1 || { tail -30 "$out/cmake-build.log"; die "library build failed"; }

# --- 2. title objects ----------------------------------------------------------
say "step 2/6: CRT and main"
cxx=("$sdk/bin/prospero-clang++" -std=c++20 -O2 -g -Wall -Wextra -Werror
     -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -I "$root/include")
"${cxx[@]}" -fno-builtin -c "$here/title/crt/rcomp_title_crt.cpp" -o "$out/obj/crt.o"
"${cxx[@]}" -c "$here/libc_compat/time_utc.cpp" -o "$out/obj/time_utc.o"
"${cxx[@]}" ${title_defs[@]+"${title_defs[@]}"} \
    -DRCOMP_TITLE_ID="\"$title_id\"" -DRCOMP_TITLE_BUILD_ID="\"$build_id\"" \
    -c "$here/title/main.cpp" -o "$out/obj/main.o"

# --- 3. intermediate PIE -------------------------------------------------------
say "step 3/6: link intermediate PIE"
"$sdk/bin/prospero-lld" -T "$here/title/crt/rcomp_title.ld" \
    --version-script "$here/title/crt/rcomp_title_symbols.map" --exclude-libs=ALL \
    --gc-sections --error-limit=0 -Map "$out/title.map" -e _start \
    -L "$sdk/target/lib" \
    -o "$out/title.pie.elf" \
    "$out/obj/crt.o" "$out/obj/main.o" "$out/obj/time_utc.o" --start-group \
    ${corpus_archives[@]+"${corpus_archives[@]}"} ${m3_archives[@]+"${m3_archives[@]}"} \
    ${xex_archives[@]+"${xex_archives[@]}"} ${xcp_archives[@]+"${xcp_archives[@]}"} "$lib_build/librcomp_platform_selftest.a" "$lib_build/librcomp_platform.a" --end-group \
    --as-needed "$sdk/target/lib/libSceLibcInternal.so" "$sdk/target/lib/libkernel.so" \
    ${xex_shared[@]+"${xex_shared[@]}"} \
    ${runtime_shared[@]+"${runtime_shared[@]}"} \
    >"$out/link.log" 2>&1 || { cat "$out/link.log"; die "link failed"; }
if command -v llvm-readelf >/dev/null; then
    no_wx_load_segments "$out/title.pie.elf" ||
        die "intermediate PIE has a writable+executable segment"
fi
say "intermediate PIE: $out/title.pie.elf ($(stat -c %s "$out/title.pie.elf") bytes)"

# --- 4-6. conversion, signing, runtime module: external GPL-3 tools -----------
if [[ ${RCOMP_ACCEPT_GPL_TITLE_TOOLS:-0} != 1 ]]; then
    blocked "steps 4-6 (ELF->PS5 conversion, fake-SELF signing, libc.prx) need the GPL-3.0
         host tools from PS5_RetroArch@${ra_pin:0:12} tooling/native. No other public source
         for them is pinned in this project. Set RCOMP_ACCEPT_GPL_TITLE_TOOLS=1 once PRIME has
         accepted building and running them (they are compiled into build/, not vendored).
         Produced so far: $out/title.pie.elf"
fi
[[ -d $ra/tooling/native ]] || blocked "no PS5_RetroArch checkout at $ra (RCOMP_DEPS)"
actual_pin=$(git -C "$ra" rev-parse HEAD 2>/dev/null || echo none)
[[ $actual_pin == "$ra_pin" ]] || blocked "PS5_RetroArch is at $actual_pin, expected $ra_pin"
git -C "$ra" diff --quiet -- tooling/native ||
    blocked "PS5_RetroArch tooling/native has local modifications"

say "step 4/6: host tools from PS5_RetroArch@${ra_pin:0:12} (GPL-3.0, built in $tools_build)"
mkdir -p "$tools_build"
native="$ra/tooling/native"
stamp=$( (cat "$native"/*.cpp "$native"/*.hpp; clang++ --version) | sha256sum | cut -c1-64)
if [[ ! -x $tools_build/ps5-native-tool || ! -x $tools_build/libc-builder ||
      $(cat "$tools_build/.stamp" 2>/dev/null) != "$stamp" ]]; then
    clang++ -std=c++20 -O2 -Wall -Wextra -Werror "$native/libc_builder.cpp" \
        -o "$tools_build/libc-builder"
    clang++ -std=c++20 -O2 -Wall -Wextra -Werror "$native/native_app_builder.cpp" \
        "$native/self_container.cpp" "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        -lz -o "$tools_build/ps5-native-tool" ||
        { echo "(needs zlib development files)" >&2; exit 2; }
    printf '%s\n' "$stamp" >"$tools_build/.stamp"
fi
tool="$tools_build/ps5-native-tool"
"$tools_build/libc-builder" "$native/runtime/api-surface.txt" "$native/runtime/imports.txt" \
    "$tools_build/libc.raw.elf" >/dev/null
"$tool" self --sign --in "$tools_build/libc.raw.elf" --out "$tools_build/libc.prx" >/dev/null
[[ $(sha256sum "$tools_build/libc.raw.elf" | cut -d' ' -f1) == "$libc_raw_sha" ]] ||
    die "libc.raw.elf digest differs from the PS5_RetroArch release"
[[ $(sha256sum "$tools_build/libc.prx" | cut -d' ' -f1) == "$libc_prx_sha" ]] ||
    die "libc.prx digest differs from the PS5_RetroArch release"

say "step 5/6: convert and sign"
"$tool" link --in "$out/title.pie.elf" --out "$out/eboot.elf" \
    --stub-dir "$sdk/target/lib" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf >"$out/convert.log" 2>&1 ||
    { cat "$out/convert.log"; die "conversion failed"; }
if command -v llvm-readelf >/dev/null; then
    no_wx_load_segments "$out/eboot.elf" ||
        die "converted PS5 ELF has a writable+executable segment"
fi
# A title process loads libkernel.sprx, never libkernel_web: imports bound to
# libkernel_web stay NULL and the title jumps to 0 on its first kernel call
# (PS5 run of kit f414844, docs/PS5_RESULTS.md).
if grep -aq 'libkernel_web' "$out/eboot.elf"; then
    die "eboot.elf imports libkernel_web (payload-only module); link libkernel.so"
fi

app="$out/dist/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic "$fself_magic" \
    >"$out/sign.log" 2>&1 || { cat "$out/sign.log"; die "signing failed"; }

say "step 6/6: assemble $app"
cp "$param" "$app/sce_sys/param.json"
if (( m3 )); then
    mkdir -p "$app/fixtures/m3/data"
    cp "$root/fixtures/ppc/m3/data/m3.bin" "$app/fixtures/m3/data/m3.bin"
fi
if (( xex )); then
    mkdir -p "$app/fixtures/xex"
    cp "$xex_file" "$app/fixtures/xex/rcomp_title.xex"
    if (( xex_packaged_boot || xex_services )); then
        cp "$xex_boot_data" "$app/fixtures/xex/boot-data.bin"
    fi
fi
cp "$tools_build/libc.prx" "$app/sce_module/libc.prx"
# 512x512 launcher icon generated here (plain colour; no third-party asset).
python3 - "$app/sce_sys/icon0.png" <<'PY'
import struct, sys, zlib
w = h = 512
row = b"\x00" + bytes((0x1E, 0x5A, 0x96)) * w
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(row * h, 9)) + chunk(b"IEND", b""))
open(sys.argv[1], "wb").write(png)
PY
"$tool" self --inspect --file "$app/eboot.bin" >"$out/inspect-eboot.txt"
"$tool" self --inspect --file "$app/sce_module/libc.prx" >"$out/inspect-libc.txt"
(cd "$app" && find . -type f ! -name manifest.sha256 -print0 | sort -z |
    xargs -0 sha256sum >manifest.sha256)
say "built $app ($(find "$app" -type f | wc -l) files, eboot.bin $(stat -c %s "$app/eboot.bin") bytes)"
say "PS5 execution: NOT TESTED (use platform/ps5/tools/run_title.sh with explicit console settings)"

if (( package )); then
    blocked "--package: .ffpkg needs UFS2Tool (.NET 8 SDK) and .ffpfsc needs MkPFS (Python,
         network fetch); neither is available/pinned here. The folder above is the deliverable."
fi
