#!/usr/bin/env bash
# R-comp title link against one pinned public RADV archive and actual platform
# runtime. No old PS5_Vulkan backend or Emu-3 artifact participates.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
base="$root/build/vulkan-gta-radv-20260928"
sdk="$base/ps5-sdk"
driver="$base/radv-ps5/src/amd/vulkan/libvulkan_radeon.a"
builtins="$base/compiler-rt-ps5/lib/freebsd/libclang_rt.builtins-x86_64.a"
platform="$sdk/target/lib/libps5platform.a"
out=${RCOMP_RADV_PROBES_OUT:-$base/rcomp-probes}
# The PS5 title id and display name of the linked application. Each recompiled game is an application of its own and two games installed together need two ids
# (GTA IV keeps the default PPSA88360; Episodes from Liberty City is built with RCOMP_TITLE_ID=PPSA88361 RCOMP_TITLE_NAME="Grand Theft Auto: Episodes from Liberty City").
title_id=${RCOMP_TITLE_ID:-PPSA88360}
title_name=${RCOMP_TITLE_NAME:-R-comp platform test}
[[ $title_id =~ ^PPSA[0-9]{5}$ ]] || { echo "bad RCOMP_TITLE_ID: $title_id" >&2; exit 2; }
[[ $title_name =~ ^[A-Za-z0-9\ :._-]+$ ]] || { echo "bad RCOMP_TITLE_NAME: $title_name" >&2; exit 2; }
[[ $out == "$root"/build/* ]] || { echo 'probe output must be under R-comp/build' >&2; exit 2; }
headers="$root/build/prime-vulkan-headers/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include"
export PATH=/usr/bin:/bin
export LLVM_CONFIG=/usr/bin/llvm-config
for file in "$driver" "$builtins" "$platform"; do
    [[ -f $file ]] || { echo "BLOCKED missing $file" >&2; exit 2; }
done
mkdir -p "$out/obj"
llvm-nm --defined-only "$builtins" > "$out/compiler-rt-defined.txt"
grep -Eq ' [TW] __emutls_get_address$' "$out/compiler-rt-defined.txt"
python3 - "$base/src/PS5_Vulkan/tools/radv-link.sh" "$out/wraps.rsp" <<'PY'
import re,sys
from pathlib import Path
text=Path(sys.argv[1]).read_text()
wraps=set(re.findall(r'--wrap=([A-Za-z0-9_]+)',text))
aliases=set()
for block in re.finditer(r'for name in (.*?); do\s*(.*?)\s*done',text,re.S):
    names=block[1].replace('\\','').split()
    assert all(re.fullmatch('[A-Za-z0-9_]+',name) for name in names)
    if '--wrap=$name' in block[2]:wraps.update(names)
    elif '--defsym=$name=ps5_$name' in block[2]:aliases.update(names)
assert 'pthread_create' in wraps and 'malloc' in wraps and 'gmtime_r' in aliases
flags=['--wrap='+name for name in sorted(wraps)]
for name in sorted(aliases):
    target='rcomp_radv_'+name if name in ('statvfs','fstatvfs') else 'ps5_'+name
    flags.append('--defsym='+name+'='+target)
Path(sys.argv[2]).write_text('\n'.join(flags)+'\n')
print('Pinned platform wrap/alias symbols:',len(wraps),len(aliases))
PY
sdk_libs=()
for file in "$sdk/target/lib/"*.so; do
    [[ ${file##*/} == libkernel_web.so || ${file##*/} == libkernel.so || ${file##*/} == libSceLibcInternal.so ]] && continue
    sdk_libs+=("$file")
done
runtime_libs=("$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a")
for file in "${runtime_libs[@]}"; do test -f "$file";done
c=("$sdk/bin/prospero-clang" -std=gnu11 -O2 -g -fPIC -fno-stack-protector -ffunction-sections -fdata-sections
    -DRCOMP_VK_STATIC_LINK -DRCOMP_VK_RADV -DRCOMP_TARGET_PS5=1 -I"$headers" -I"$root/gpu/vulkan/common"
    -I"$root/gpu/vulkan/draw_test/generated")
cxx=("$sdk/bin/prospero-clang++" -std=c++20 -O2 -g -fPIC -fno-exceptions -fno-rtti -fno-stack-protector
     -ffunction-sections -fdata-sections)
"${cxx[@]}" -fno-builtin -c "$root/platform/ps5/title/crt/rcomp_title_crt.cpp" -o "$out/obj/crt.o"
"${c[@]}" -c "$root/gpu/vulkan/common/rcvk_loader.c" -o "$out/obj/loader.o"
"${c[@]}" -c "$root/gpu/vulkan/ps5/radv_icd_bridge.c" -o "$out/obj/bridge.o"
"${c[@]}" -c "$root/gpu/vulkan/ps5/radv_platform_limits.c" -o "$out/obj/limits.o"
mkdir -p "$out/stubs"
for item in 'libSceAgc:agc_canary_link_stub.c' 'libSceAgcDriver:agc_driver_canary_link_stub.c'; do
    library=${item%%:*};file=${item#*:}
    "${c[@]}" -c "$base/src/PS5_Vulkan/vendor/ps5/sdk/stubs/$file" -o "$out/obj/$library.o"
    "$sdk/bin/prospero-lld" --shared -soname "$library.prx" \
        -o "$out/stubs/$library.so" "$out/obj/$library.o"
done
tool="$root/build/platform-ps5-tools/ps5-native-tool"
test -x "$tool"
no_wx() {
    python3 - "$1" <<'PY'
import pathlib,sys
rows=[line.split() for line in pathlib.Path(sys.argv[1]).read_text().splitlines()
      if line.lstrip().startswith('LOAD ')]
assert rows, 'missing LOAD segments'
assert all(not ('W' in ''.join(r[6:-1]) and 'E' in ''.join(r[6:-1])) for r in rows), 'W+X LOAD segment'
PY
}
# game: exploratory (non-release) recompiled-title link; needs RCOMP_RADV_GAME_BUILD
# (CMake build dir of app/m6 with the archives) and RCOMP_RADV_GAME_XEX.
if (( $# )); then modes=("$@");else modes=(probe draw);fi
for mode in "${modes[@]}"; do
    [[ $mode == probe || $mode == draw || $mode == game ]] || { echo 'mode must be probe, draw or game';exit 2; }
    dir="$out/$mode"
    mkdir -p "$dir/obj"
    game_libs=();game_objs=();test_obj=()
    if [[ $mode == game ]]; then
        gb=${RCOMP_RADV_GAME_BUILD:?RCOMP_RADV_GAME_BUILD required for game};gx=${RCOMP_RADV_GAME_XEX:?RCOMP_RADV_GAME_XEX required}
        [[ -f $gx ]] || { echo "BLOCKED missing $gx" >&2;exit 2; }
        source_file="$root/gpu/vulkan/ps5/radv_title_main.c";defs=(-DRCOMP_RADV_GAME=1 -Drcomp_radv_test_main=rcomp_program_main)
        game_libs=("$gb/librcomp_m6_title.a" "$gb/librcomp_m6_generated.a" "$gb/app/librcomp_app.a"
            "$gb/runtime/librcomp_runtime_video.a" "$gb/runtime/librcomp_runtime.a"
            "$gb/platform/librcomp_platform_input.a"
            "$gb/xenos/librcomp_xenos_vulkan.a" "$gb/xenos/librcomp_xenos_host.a" "$gb/xenos/librcomp_xenos_cp.a"
            "$gb/xenos/librcomp_xenos_shader.a" "$gb/xenos/librcomp_rex_base.a" "$gb/xenos/librcomp_glslang_spirv.a"
            "$gb/xenos/glslang/SPIRV/libSPIRV.a" "$gb/xenos/glslang/glslang/libglslang.a"
            "$gb/xenos/glslang/glslang/libMachineIndependent.a" "$gb/xenos/glslang/glslang/libGenericCodeGen.a"
            "$gb/platform/librcomp_platform.a")
        # Mirror the runtime's optional independent XMA decoder dependency.
        # An empty/missing cache entry preserves the codec-disabled title link.
        xma_ffmpeg_root=""
        if [[ -f $gb/CMakeCache.txt ]]; then
            xma_ffmpeg_root=$(sed -n 's/^RCOMP_XMA_FFMPEG_ROOT:[^=]*=//p' "$gb/CMakeCache.txt")
            xma_ffmpeg_root=${xma_ffmpeg_root%$'\r'}
        fi
        if [[ -n $xma_ffmpeg_root ]]; then
            game_libs+=("$xma_ffmpeg_root/lib/libavcodec.a" "$xma_ffmpeg_root/lib/libavutil.a")
        fi
        for a in "${game_libs[@]}";do [[ -f $a ]] || { echo "BLOCKED missing $a" >&2;exit 2; };done
        # Audio is optional for existing codec-only titles. A runtime/title
        # requesting the platform API must have its actual backend archive.
        audio_lib="$gb/platform/librcomp_platform_audio.a"
        if [[ -f $audio_lib ]]; then
            game_libs+=("$audio_lib")
        else
            llvm-nm --undefined-only "${game_libs[@]}" > "$dir/audio-references.txt"
            if grep -Eq ' U rcomp_audio_(open|submit|close)$' "$dir/audio-references.txt"; then
                echo "BLOCKED platform audio requested but missing $audio_lib" >&2
                exit 2
            fi
        fi
        while IFS= read -r a;do game_libs+=("$a");done < <(find "$gb/xenos/glslang" -name libOSDependent.a -o -name libOGLCompiler.a)
        sha256sum "${game_libs[@]}" > "$dir/link-inputs.sha256"
        for f in time_utc c_locale bit_ops;do
            "${cxx[@]}" -std=c++17 -Wall -Wextra -Werror -c "$root/platform/ps5/libc_compat/$f.cpp" -o "$dir/obj/$f.o"
            game_objs+=("$dir/obj/$f.o")
        done
    elif [[ $mode == probe ]]; then source_file="$root/gpu/vulkan/probe/vk_probe.c";defs=(-DRCOMP_RADV_PROBE=1)
    else source_file="$root/gpu/vulkan/draw_test/draw_test.c";defs=();fi
    if [[ $mode != game ]]; then
        "${c[@]}" -Dmain=rcomp_radv_test_main -c "$source_file" -o "$dir/obj/test.o"
        test_obj=("$dir/obj/test.o")
    fi
    id=$( (printf '%s\n' "${c[@]}" "${cxx[@]}" "$mode" "$title_id";
        cat "$source_file" "$root/gpu/vulkan/ps5/radv_title_main.c" \
            "$root/gpu/vulkan/ps5/radv_platform_limits.c" \
            "$root/platform/ps5/title/crt/rcomp_title_crt.cpp" \
            "$root/platform/ps5/title/crt/rcomp_title.ld" \
            "$out/wraps.rsp" "$driver" "$builtins" "$platform" ${RCOMP_LINK_ORDER_FILE:+"$RCOMP_LINK_ORDER_FILE"} ${game_libs[@]+"${game_libs[@]}"}) | sha256sum | cut -c1-16)
    # RCOMP_RADV_MAIN_CFLAGS: extra flags for the title main (the game build passes the TLS model
    # its archives use, since main.o shares thread-local diagnostic variables with them).
    "${c[@]}" ${RCOMP_RADV_MAIN_CFLAGS:-} "${defs[@]}" -DRCOMP_RADV_BUILD_ID="\"$id\"" -DRCOMP_TITLE_ID="\"$title_id\"" \
        -c "$root/gpu/vulkan/ps5/radv_title_main.c" -o "$dir/obj/main.o"
    # RCOMP_LINK_ORDER_FILE (optional): a lld symbol-ordering file (tools/hot_function_order.py, from PC-sampler runs): the recompiled
    # functions the guest actually runs, hottest first, laid out together at the start of .text. A layout change only.
    order_args=()
    if [[ -n ${RCOMP_LINK_ORDER_FILE:-} ]]; then
        [[ -f $RCOMP_LINK_ORDER_FILE ]] || { echo "RCOMP_LINK_ORDER_FILE not found: $RCOMP_LINK_ORDER_FILE"; exit 1; }
        order_args=(--symbol-ordering-file "$RCOMP_LINK_ORDER_FILE" --no-warn-symbol-ordering)
    fi
    "$sdk/bin/prospero-lld" -T "$root/platform/ps5/title/crt/rcomp_title.ld" \
        --version-script "$root/platform/ps5/title/crt/rcomp_title_symbols.map" \
        --exclude-libs=ALL --gc-sections --eh-frame-hdr --error-limit=0 -L "$sdk/target/lib" ${order_args[@]+"${order_args[@]}"} \
        --no-dynamic-linker -z nodynamic-undefined-weak -Map "$dir/title.map" \
        -e _start -o "$dir/title.pie.elf" "$out/obj/crt.o" "$dir/obj/main.o" ${test_obj[@]+"${test_obj[@]}"} ${game_objs[@]+"${game_objs[@]}"} \
        "$out/obj/loader.o" "$out/obj/bridge.o" "$out/obj/limits.o" \
        "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" \
        "@$out/wraps.rsp" --whole-archive "$driver" --no-whole-archive \
        --start-group ${game_libs[@]+"${game_libs[@]}"} "$platform" "${runtime_libs[@]}" "$builtins" --end-group \
        --as-needed "$sdk/target/lib/libkernel.so" "$sdk/target/lib/libSceLibcInternal.so" "${sdk_libs[@]}" \
        > "$dir/link.log" 2>&1 || { tail -100 "$dir/link.log";exit 1; }
    llvm-readelf --program-headers --dynamic "$dir/title.pie.elf" > "$dir/readelf.txt"
    no_wx "$dir/readelf.txt"
    if grep -q 'libkernel_web' "$dir/readelf.txt";then echo 'invalid payload module in title';exit 1;fi
    app="$dir/dist/$title_id"
    mkdir -p "$app/sce_sys" "$app/sce_module"
    "$tool" link --in "$dir/title.pie.elf" --out "$dir/eboot.elf" --stub-dir "$sdk/target/lib" \
        --stub "$out/stubs/libSceAgc.so" --stub "$out/stubs/libSceAgcDriver.so" \
        --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > "$dir/convert.log" 2>&1 ||
        { tail -70 "$dir/convert.log";exit 1; }
    if [[ $mode == game ]]; then
        python3 "$root/gpu/vulkan/ps5/radv_audio_imports.py" "$dir/title.pie.elf" "$dir/eboot.elf" \
            "$root/build/platform-audioout-20260930/exports-9.40.json" "$dir/audio-imports.json"
    fi
    llvm-readelf --program-headers --dynamic "$dir/eboot.elf" > "$dir/converted-readelf.txt"
    no_wx "$dir/converted-readelf.txt"
    "$tool" self --sign --in "$dir/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$dir/sign.log" 2>&1
    sed -e "s/PPSA88360/$title_id/g" -e "s/\"conceptId\": \"88360\"/\"conceptId\": \"${title_id#PPSA}\"/" \
        -e "s/R-comp platform test/$title_name/" "$root/platform/ps5/title/sce_sys/param.json" > "$app/sce_sys/param.json"
    cp "$root/build/platform-ps5-title-kernel-boot/dist/PPSA88360/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
    cp "$root/build/platform-ps5-tools/libc.prx" "$app/sce_module/libc.prx"
    if [[ $mode == game ]];then mkdir -p "$app/image";cp "$gx" "$app/image/plain.xex";fi
    (cd "$app" && find . -type f ! -name manifest.sha256 -print0 | sort -z | xargs -0 sha256sum > manifest.sha256)
    "$tool" self --inspect --file "$app/eboot.bin" > "$dir/self.log"
    printf 'PASS linked R-comp RADV %s title build=%s; PS5 execution NOT TESTED\n' "$mode" "$id"
    printf '%s\n' "$id" > "$dir/BUILD_ID.txt"
    sha256sum "$app/eboot.bin"
done
