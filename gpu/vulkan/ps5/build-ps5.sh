#!/usr/bin/env bash
# R-comp - Agent 4 (PS5_Vulkan integration).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-build rcomp_vk_probe and rcomp_vk_draw_test for the PS5 with
# PS5_Vulkan linked statically, reproducing the link PS5_RetroArch@18dc105
# performs for its Vulkan title (LINK_CONTRACT.md). Inputs are snapshots only:
#
#   ART/driver          snapshot_manifest.py --profile driver of a PS5_Vulkan build root
#   ART/retroarch-link  snapshot of PS5_RetroArch tooling/native, tooling/ps5-stubs,
#                       tooling/prospero-clang18 (and the scripts, for reference)
#   SDK                 a copy of ps5-payload-sdk v0.42
#
#   ps5/build-ps5.sh ART SDK OUT
#
# Produces, per program, OUT/<name>.elf (prospero-lld output), OUT/<name>.map,
# OUT/<name>/eboot.elf (native converter) and OUT/<name>/eboot.bin (fake-signed
# SELF). It does NOT assemble a title folder (param.json, sce_sys, libc.prx):
# that is the platform lot's packaging. Nothing here has run on a console.
set -euo pipefail

art=$(cd -- "${1:?ART}" && pwd)
sdk=$(cd -- "${2:?SDK}" && pwd)
out=${3:?OUT}
mkdir -p "$out"
out=$(cd -- "$out" && pwd)
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
drv="$art/driver"
ra="$art/retroarch-link"
native="$ra/tooling/native"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}
export PS5_PAYLOAD_SDK=$sdk
export PS5_DISABLE_CCACHE=1
cc() { sh "$ra/tooling/prospero-clang18" "$@"; }

for f in "$drv/MANIFEST.json" "$ra/MANIFEST.json"; do
    python3 - "$f" <<'PY' || { echo "BLOCKED: snapshot $f is not valid" >&2; exit 2; }
import json, sys
sys.exit(0 if json.load(open(sys.argv[1]))["valid"] else 1)
PY
done
# Re-verify every snapshot byte before linking with it.
(cd "$drv" && sha256sum --quiet --check SHA256SUMS) || { echo "FAIL: driver snapshot modified" >&2; exit 1; }
(cd "$ra" && sha256sum --quiet --check SHA256SUMS) || { echo "FAIL: recipe snapshot modified" >&2; exit 1; }

# --- the contract's inputs (LINK_CONTRACT.md §2, order as tools/build-title.sh) --
vulkan_archives=(
    "$drv/build/driver/ps5/libps5vk.ps5.a"
    "$drv/.deps/native/vulkan-runtime/lib/libvk_runtime.ps5.a"
    "$drv/build/driver/ps5/libpsbc_driver.ps5.a"
    "$drv/.deps/native/psbc/lib/libpsbc_support.ps5.a"
)
mesa_util=("$drv/build/rcomp-mesa-util/u_thread.o" "$drv/build/rcomp-mesa-util/anon_file.o"
           "$drv/build/rcomp-mesa-util/os_file.o")
vk_headers="$drv/.deps/work/psbc-ps5/third_party/Vulkan-Headers/include"
builtins="$("$PS5_CLANG" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
cxx_runtime=("$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a")
for f in "${vulkan_archives[@]}" "${mesa_util[@]}" "${cxx_runtime[@]}" "$vk_headers/vulkan/vulkan.h" \
         "$native/ps5-pie.ld" "$native/app_crt.cpp"; do
    [[ -e $f ]] || { echo "BLOCKED: missing $f" >&2; exit 2; }
done
[[ -f $builtins ]] || { echo "note: no $builtins; linking without it (as PS5_RetroArch allows)" >&2; builtins=; }

obj="$out/obj"
mkdir -p "$obj" "$out/host"
# --- the host converter/signer, from PS5_RetroArch's own tooling/native -------
tool="$out/host/ps5-native-tool"
zlib_inc=${ZLIB_INCLUDE:-}
zlib_a=${ZLIB_ARCHIVE:-}
if [[ -z $zlib_a ]]; then
    echo "BLOCKED: set ZLIB_INCLUDE/ZLIB_ARCHIVE (a static host zlib; PS5_RetroArch and PS5_Vulkan build 1.3.2)" >&2
    exit 2
fi
clang++-18 -std=c++20 -O2 -Wall -Wextra -Werror -I "$zlib_inc" \
    "$native/native_app_builder.cpp" "$native/self_container.cpp" "$native/elf_object.cpp" \
    "$native/sce_module_writer.cpp" "$zlib_a" -o "$tool"

# --- CRT, C++ runtime shims and AGC import stubs (PS5_RetroArch tools/build.sh) ---
cxxflags=(-std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
cc "${cxxflags[@]}" -c "$native/app_crt.cpp" -o "$obj/app_crt.o"
cc "${cxxflags[@]}" -c "$native/app_cpp_runtime.cpp" -o "$obj/app_cpp_runtime.o"
stubs=()
for entry in "libSceAgc agc_link_stub.c" "libSceAgcDriver agc_driver_link_stub.c"; do
    read -r lib src <<< "$entry"
    cc -std=c11 -O2 -fPIC -ffunction-sections -fdata-sections -c "$ra/tooling/ps5-stubs/$src" \
        -o "$obj/${lib}_link_stub.o"
    mkdir -p "$out/stubs"
    "$sdk/bin/prospero-lld" --shared -soname "${lib}.prx" -o "$out/stubs/${lib}.so" "$obj/${lib}_link_stub.o"
    stubs+=("$out/stubs/${lib}.so")
done

cflags=(-std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -DRCOMP_TARGET_PS5
        -I "$vk_headers" -I "$here/common" -I "$here/draw_test/generated")
cc "${cflags[@]}" -c "$here/common/rcvk_loader.c" -o "$obj/rcvk_loader.o"
# Title wrapper (RCOMP_TITLE_WRAP=1, default): each program's main becomes
# rcomp_program_main and common/rcomp_title_wrap.c logs to the title folder
# with RCOMP-TITLE begin/end framing (judged by platform/ps5/tools/run_title.sh).
title_id=${RCOMP_TITLE_ID:-PPSA88360}
wrapdef=()
if [[ ${RCOMP_TITLE_WRAP:-1} == 1 ]]; then
    wrapdef=(-Dmain=rcomp_program_main)
    for p in rcomp_vk_probe rcomp_vk_draw_test rcomp_m5; do
        cc "${cflags[@]}" -DRCOMP_TITLE_ID="\"$title_id\"" -DRCOMP_PROGRAM_NAME="\"$p\"" \
            -c "$here/common/rcomp_title_wrap.c" -o "$obj/wrap_$p.o"
    done
    # Experimental-profile variants (PS5_Vulkan fork: device 1.1): the probe,
    # and the Xenos GPU backend, which needs that profile's features.
    for p in rcomp_vk_probe_exp rcomp_xenos_vk rcomp_xex_gfx rcomp_m6; do
        cc "${cflags[@]}" -DRCOMP_TITLE_ID="\"$title_id\"" -DRCOMP_PROGRAM_NAME="\"$p\"" \
            -DRCOMP_VK_PROFILE=1 \
            -c "$here/common/rcomp_title_wrap.c" -o "$obj/wrap_$p.o"
    done
fi
wrap() { [[ ${RCOMP_TITLE_WRAP:-1} == 1 ]] && echo "$obj/wrap_$1.o"; }
cc "${cflags[@]}" ${wrapdef[@]+"${wrapdef[@]}"} -c "$here/probe/vk_probe.c" -o "$obj/vk_probe.o"
cc "${cflags[@]}" ${wrapdef[@]+"${wrapdef[@]}"} -c "$here/draw_test/draw_test.c" -o "$obj/draw_test.o"

# Archive grouping. RCOMP_VK_LINK_CONTRACT selects whose proven link we copy:
#   retroarch (default): PS5_RetroArch@18dc105 tools/build-title.sh, all four
#     driver archives --whole-archive (public PS5_Vulkan @9639c418);
#   ps5vk-driver: the driver's own link (PS5_Vulkan fork @17350536
#     tools/build-driver.sh:306-313): libps5vk + libvk_runtime whole, the
#     compiler archives normal. Needed because that revision defines
#     vk_nir_convert_ycbcr in both libvk_runtime and libpsbc_driver.
case "${RCOMP_VK_LINK_CONTRACT:-retroarch}" in
    retroarch) driver_link=(--whole-archive "${vulkan_archives[@]}" --no-whole-archive) ;;
    ps5vk-driver)
        # Title adaptation of the driver copies (see ps5vk_title_shims.c): the
        # snapshot is left untouched, the renamed copies live in $obj/adapt.
        adapt="$obj/adapt"; mkdir -p "$adapt"
        oc="$sdk/bin/prospero-objcopy"
        for i in 0 1 2 3; do cp "${vulkan_archives[$i]}" "$adapt/"; done
        a_vk="$adapt/$(basename "${vulkan_archives[0]}")"; a_rt="$adapt/$(basename "${vulkan_archives[1]}")"
        a_pd="$adapt/$(basename "${vulkan_archives[2]}")"; a_ps="$adapt/$(basename "${vulkan_archives[3]}")"
        for a in "$a_vk" "$a_rt"; do
            "$oc" --redefine-sym getenv=rcomp_vk_getenv --redefine-sym sceAgcInit=rcomp_vk_agc_init \
                --redefine-sym pthread_attr_setstack=rcomp_vk_pthread_attr_setstack "$a"
        done
        "$oc" --redefine-sym getenv=rcomp_vk_getenv --redefine-sym setenv=rcomp_vk_setenv \
            --redefine-sym unsetenv=rcomp_vk_unsetenv --redefine-sym mkstemp=rcomp_vk_mkstemp \
            --redefine-sym pthread_mutex_timedlock=rcomp_vk_mutex_timedlock \
            --redefine-sym system=rcomp_vk_system --redefine-sym getrlimit=rcomp_vk_getrlimit "$a_pd"
        "$oc" --redefine-sym usleep=rcomp_vk_usleep "$a_ps"
        cc -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
            -c "$here/ps5/ps5vk_title_shims.c" -o "$obj/ps5vk_title_shims.o"
        driver_link=("$obj/ps5vk_title_shims.o" --whole-archive "$a_vk" "$a_rt"
                     --no-whole-archive "$a_pd" "$a_ps") ;;
    *) echo "unknown RCOMP_VK_LINK_CONTRACT=$RCOMP_VK_LINK_CONTRACT" >&2; exit 2 ;;
esac
echo "link contract: ${RCOMP_VK_LINK_CONTRACT:-retroarch}" >&2
link() {
    local name=$1; shift
    local inputs=("$obj/app_crt.o" "$obj/app_cpp_runtime.o" "$@" "$obj/rcvk_loader.o" "${stubs[@]}"
        "${driver_link[@]}" "${mesa_util[@]}"
        -L "$sdk/target/lib" --start-group "${cxx_runtime[@]}" ${builtins:+"$builtins"} --end-group)
    "$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr --error-limit=0 \
        --Map="$out/$name.map" \
        --no-dynamic-linker -z nodynamic-undefined-weak \
        --version-script "$native/app-symbols.map" \
        --exclude-libs=ALL -e _start -o "$out/$name.elf" "${inputs[@]}" \
        --as-needed "$sdk"/target/lib/*.so
    mkdir -p "$out/$name"
    "$tool" link --in "$out/$name.elf" --out "$out/$name/eboot.elf" \
        --stub-dir "$sdk/target/lib" --stub "${stubs[0]}" --stub "${stubs[1]}" \
        --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
    "$tool" self --sign --in "$out/$name/eboot.elf" --out "$out/$name/eboot.bin" --magic 0x1D3D154F
    echo "built $name: $(sha256sum "$out/$name/eboot.bin" | cut -d' ' -f1) eboot.bin ($(stat -c %s "$out/$name/eboot.bin") bytes)"
}
link rcomp_vk_probe "$obj/vk_probe.o" $(wrap rcomp_vk_probe)
link rcomp_vk_draw_test "$obj/draw_test.o" $(wrap rcomp_vk_draw_test)
[[ ${RCOMP_TITLE_WRAP:-1} == 1 && ${RCOMP_VK_LINK_CONTRACT:-retroarch} == ps5vk-driver ]] && link rcomp_vk_probe_exp "$obj/vk_probe.o" "$obj/wrap_rcomp_vk_probe_exp.o"
# M5 integrated slice (optional): RCOMP_M5_BUILD = a PS5 build of tests/m5.
if [[ -n ${RCOMP_M5_BUILD:-} ]]; then
    m5=("$RCOMP_M5_BUILD/librcomp_m5_selftest.a" "$RCOMP_M5_BUILD/librcomp_m5_generated.a"
        "$RCOMP_M5_BUILD/librcomp_gfx.a" "$RCOMP_M5_BUILD/runtime/librcomp_runtime.a"
        "$RCOMP_M5_BUILD/platform/librcomp_platform.a")
    for a in "${m5[@]}"; do [[ -f $a ]] || { echo "BLOCKED: missing $a" >&2; exit 2; }; done
    cc -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
        -c "$here/../../tests/m5/m5_title_main.cpp" -o "$obj/m5_main.o"
    link rcomp_m5 "$obj/m5_main.o" $(wrap rcomp_m5) --start-group "${m5[@]}" --end-group
fi
# Xenos GPU (rexglue Vulkan backend) on PS5_Vulkan (optional):
# RCOMP_XENOS_VK_BUILD = a PS5 build of tests/xenos_cp (rcomp_xenos_vulkan_selftest).
if [[ -n ${RCOMP_XENOS_VK_BUILD:-} ]]; then
    xb=$RCOMP_XENOS_VK_BUILD
    xvk=("$xb/librcomp_xenos_vulkan_selftest.a" "$xb/xenos/librcomp_xenos_vulkan.a"
         "$xb/xenos/librcomp_xenos_host.a" "$xb/xenos/librcomp_xenos_cp.a" "$xb/xenos/librcomp_xenos_shader.a"
         "$xb/xenos/librcomp_rex_base.a" "$xb/xenos/librcomp_glslang_spirv.a"
         "$xb/xenos/glslang/SPIRV/libSPIRV.a" "$xb/xenos/glslang/glslang/libglslang.a"
         "$xb/xenos/glslang/glslang/libMachineIndependent.a" "$xb/xenos/glslang/glslang/libGenericCodeGen.a"
         "$xb/platform/librcomp_platform.a")
    for a in "${xvk[@]}"; do [[ -f $a ]] || { echo "BLOCKED: missing $a" >&2; exit 2; }; done
    while IFS= read -r a; do xvk+=("$a"); done < <(find "$xb/xenos/glslang" -name 'libOSDependent.a' -o -name 'libOGLCompiler.a')
    [[ ${RCOMP_VK_LINK_CONTRACT:-retroarch} == ps5vk-driver ]] ||
        { echo "BLOCKED: the Xenos backend needs the experimental profile (RCOMP_VK_LINK_CONTRACT=ps5vk-driver)" >&2; exit 2; }
    cc -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
        -c "$here/../../tests/xenos_cp/xenos_vk_title_main.cpp" -o "$obj/xenos_vk_main.o"
    cc -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -ffunction-sections \
        -c "$here/../../platform/ps5/libc_compat/time_utc.cpp" -o "$obj/time_utc.o"
    cc -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -ffunction-sections \
        -c "$here/../../platform/ps5/libc_compat/c_locale.cpp" -o "$obj/c_locale.o"
    link rcomp_xenos_vk "$obj/xenos_vk_main.o" "$obj/time_utc.o" "$obj/c_locale.o" $(wrap rcomp_xenos_vk) \
        --start-group "${xvk[@]}" --end-group
fi
# Game shell + recompiled synthetic graphics XEX (optional):
# RCOMP_XEX_GFX_BUILD = a PS5 build of tests/xex_gfx (rcomp_xex_gfx_selftest).
if [[ -n ${RCOMP_XEX_GFX_BUILD:-} ]]; then
    gb=$RCOMP_XEX_GFX_BUILD
    xg=("$gb/librcomp_xex_gfx_selftest.a" "$gb/librcomp_xex_gfx_generated.a" "$gb/app/librcomp_app.a"
        "$gb/runtime/librcomp_runtime_video.a" "$gb/runtime/librcomp_runtime.a"
        "$gb/platform/librcomp_platform_input.a"
        "$gb/xenos/librcomp_xenos_vulkan.a" "$gb/xenos/librcomp_xenos_host.a" "$gb/xenos/librcomp_xenos_cp.a"
        "$gb/xenos/librcomp_xenos_shader.a" "$gb/xenos/librcomp_rex_base.a" "$gb/xenos/librcomp_glslang_spirv.a"
        "$gb/xenos/glslang/SPIRV/libSPIRV.a" "$gb/xenos/glslang/glslang/libglslang.a"
        "$gb/xenos/glslang/glslang/libMachineIndependent.a" "$gb/xenos/glslang/glslang/libGenericCodeGen.a"
        "$gb/platform/librcomp_platform.a")
    for a in "${xg[@]}"; do [[ -f $a ]] || { echo "BLOCKED: missing $a" >&2; exit 2; }; done
    while IFS= read -r a; do xg+=("$a"); done < <(find "$gb/xenos/glslang" -name 'libOSDependent.a' -o -name 'libOGLCompiler.a')
    [[ ${RCOMP_VK_LINK_CONTRACT:-retroarch} == ps5vk-driver ]] ||
        { echo "BLOCKED: the Xenos backend needs the experimental profile (RCOMP_VK_LINK_CONTRACT=ps5vk-driver)" >&2; exit 2; }
    cc -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
        -c "$here/../../tests/xex_gfx/xex_gfx_title_main.cpp" -o "$obj/xex_gfx_main.o"
    for f in time_utc c_locale; do
        cc -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -ffunction-sections \
            -c "$here/../../platform/ps5/libc_compat/$f.cpp" -o "$obj/$f.o"
    done
    link rcomp_xex_gfx "$obj/xex_gfx_main.o" "$obj/time_utc.o" "$obj/c_locale.o" $(wrap rcomp_xex_gfx) \
        --start-group "${xg[@]}" --end-group
fi
# Externally supplied M6 XEX. The orchestrator has already decoded,
# inventoried and recompiled it; this block only accepts the resulting PS5
# archives and packages the decoded image next to the executable.
if [[ -n ${RCOMP_M6_BUILD:-} ]]; then
    mb=$RCOMP_M6_BUILD
    [[ ${RCOMP_TITLE_WRAP:-1} == 1 ]] || { echo "BLOCKED: M6 requires the PS5 parking wrapper" >&2; exit 2; }
    [[ -f ${RCOMP_M6_XEX:-} ]] || { echo "BLOCKED: RCOMP_M6_XEX is missing" >&2; exit 2; }
    m6=("$mb/librcomp_m6_title.a" "$mb/librcomp_m6_generated.a" "$mb/app/librcomp_app.a"
        "$mb/runtime/librcomp_runtime_video.a" "$mb/runtime/librcomp_runtime.a"
        "$mb/platform/librcomp_platform_input.a"
        "$mb/xenos/librcomp_xenos_vulkan.a" "$mb/xenos/librcomp_xenos_host.a" "$mb/xenos/librcomp_xenos_cp.a"
        "$mb/xenos/librcomp_xenos_shader.a" "$mb/xenos/librcomp_rex_base.a" "$mb/xenos/librcomp_glslang_spirv.a"
        "$mb/xenos/glslang/SPIRV/libSPIRV.a" "$mb/xenos/glslang/glslang/libglslang.a"
        "$mb/xenos/glslang/glslang/libMachineIndependent.a" "$mb/xenos/glslang/glslang/libGenericCodeGen.a"
        "$mb/platform/librcomp_platform.a")
    for a in "${m6[@]}"; do [[ -f $a ]] || { echo "BLOCKED: missing $a" >&2; exit 2; }; done
    while IFS= read -r a; do m6+=("$a"); done < <(find "$mb/xenos/glslang" -name 'libOSDependent.a' -o -name 'libOGLCompiler.a')
    [[ ${RCOMP_VK_LINK_CONTRACT:-retroarch} == ps5vk-driver ]] ||
        { echo "BLOCKED: M6/Xenos needs RCOMP_VK_LINK_CONTRACT=ps5vk-driver" >&2; exit 2; }
    for f in time_utc c_locale; do
        cc -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -ffunction-sections \
            -c "$here/../../platform/ps5/libc_compat/$f.cpp" -o "$obj/m6_$f.o"
    done
    link rcomp_m6 "$obj/m6_time_utc.o" "$obj/m6_c_locale.o" "$obj/wrap_rcomp_m6.o" \
        --start-group "${m6[@]}" --end-group
    mkdir -p "$out/rcomp_m6/image"
    cp -- "$RCOMP_M6_XEX" "$out/rcomp_m6/image/plain.xex"
    chmod 0400 "$out/rcomp_m6/image/plain.xex"
fi
(cd "$out" && sha256sum ./*.elf ./*/eboot.elf ./*/eboot.bin > SHA256SUMS)
if [[ -f $out/rcomp_m6/image/plain.xex ]]; then
    (cd "$out" && sha256sum rcomp_m6/image/plain.xex >> SHA256SUMS)
fi
# The snapshot must still be what was linked.
(cd "$drv" && sha256sum --quiet --check SHA256SUMS) || { echo "FAIL: driver snapshot changed during link" >&2; exit 1; }
cat "$out/SHA256SUMS"
