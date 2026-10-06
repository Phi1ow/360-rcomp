#!/usr/bin/env bash
# R-comp shelf: builds the console application, build/ps5/dist/<title id>/ (eboot.bin, sce_sys/, sce_module/).
#
#   tools/build_ps5.sh
#
# The link is R-comp's own RADV title link (rcomp gpu/vulkan/ps5/build-radv-probes.sh): the same pinned RADV
# archive, SDK, compiler-rt, platform library, wrapped symbols, title CRT and linker script, the same converter
# and signer. They are READ from an R-comp checkout (RCOMP_ROOT, default ../rcomp) and never written; everything
# this script makes goes under build/ps5. Run it in R-comp's full Cygwin (it has llvm-readelf, python3, sha256sum).
#
# Copyright (C) 2026 R-comp contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export PATH=/usr/bin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
rcomp=$(cd -- "${RCOMP_ROOT:-$root/../rcomp}" && pwd)
title_id=${RSHELF_TITLE_ID:-PPSA88300}
[[ $title_id =~ ^PPSA[0-9]{5}$ ]] || { echo "BLOCKED: RSHELF_TITLE_ID must be PPSA and five digits" >&2; exit 2; }
base="$rcomp/build/vulkan-gta-radv-20260928"
sdk="$base/ps5-sdk"
driver="$base/radv-ps5/src/amd/vulkan/libvulkan_radeon.a"
builtins="$base/compiler-rt-ps5/lib/freebsd/libclang_rt.builtins-x86_64.a"
platform="$sdk/target/lib/libps5platform.a"
headers=$(ls -d "$rcomp"/build/prime-vulkan-headers/Vulkan-Headers-*/include | head -1)
crt="$rcomp/platform/ps5/title/crt"
compat="$rcomp/platform/ps5/libc_compat"
tool="$rcomp/build/platform-ps5-tools/ps5-native-tool"
libc_prx="$rcomp/build/platform-ps5-tools/libc.prx"
stubs_src="$base/src/PS5_Vulkan/vendor/ps5/sdk/stubs"
for f in "$driver" "$builtins" "$platform" "$headers/vulkan/vulkan.h" "$crt/rcomp_title_crt.cpp" "$crt/rcomp_title.ld" \
         "$crt/rcomp_title_symbols.map" "$rcomp/gpu/vulkan/ps5/radv_platform_limits.c" "$libc_prx" \
         "$stubs_src/agc_canary_link_stub.c" "$stubs_src/agc_driver_canary_link_stub.c" "$base/src/PS5_Vulkan/tools/radv-link.sh"; do
    [[ -e $f ]] || { echo "BLOCKED: missing $f" >&2; exit 2; }
done
[[ -x $tool || -x $tool.exe ]] || { echo "BLOCKED: missing $tool" >&2; exit 2; }

out="$root/build/ps5"
obj="$out/obj"
app="$out/dist/$title_id"
rm -rf "$app"
mkdir -p "$obj" "$out/stubs" "$app/sce_sys" "$app/sce_module"
# The inputs read from R-comp, for the record.
sha256sum "$driver" "$builtins" "$platform" "$crt"/* "$compat"/{time_utc,c_locale,bit_ops}.cpp \
    "$rcomp/gpu/vulkan/ps5/radv_platform_limits.c" "$libc_prx" > "$out/rcomp-inputs.sha256"

# The platform library's wrapped and aliased symbols, read from the pinned PS5_Vulkan link (as R-comp does).
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
print('wrapped/aliased symbols:',len(wraps),len(aliases))
PY

sdk_libs=()
for file in "$sdk/target/lib/"*.so; do
    [[ ${file##*/} == libkernel_web.so || ${file##*/} == libkernel.so || ${file##*/} == libSceLibcInternal.so ]] && continue
    sdk_libs+=("$file")
done
runtime_libs=("$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a")
c=("$sdk/bin/prospero-clang" -std=gnu11 -O2 -g -fPIC -fno-stack-protector -ffunction-sections -fdata-sections -I"$headers")
build_tag=${RSHELF_BUILD_TAG:-$(cd "$root" && git rev-parse --short HEAD 2>/dev/null || echo dev)}
cxx=("$sdk/bin/prospero-clang++" -std=c++20 -O2 -g -fPIC -fno-exceptions -fno-rtti -fno-stack-protector -ffunction-sections
     -fdata-sections -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I"$headers"
     -I"$root/src/frontend" -I"$root/src/catalog" -I"$root/src/shelf"
     -DRSHELF_TITLE_ID="\"$title_id\"" -DRSHELF_BUILD_TAG="\"$build_tag\"" -DRSHELF_FONT_DIR="\"$root/assets/fonts\"")

"${cxx[@]}" -fno-builtin -Wno-unused-parameter -c "$crt/rcomp_title_crt.cpp" -o "$obj/crt.o"
"${c[@]}" -c "$rcomp/gpu/vulkan/ps5/radv_platform_limits.c" -o "$obj/limits.o"
for f in time_utc c_locale bit_ops; do
    "$sdk/bin/prospero-clang++" -std=c++17 -O2 -g -fPIC -fno-exceptions -fno-rtti -fno-stack-protector -ffunction-sections \
        -c "$compat/$f.cpp" -o "$obj/compat_$f.o"
done
for item in 'libSceAgc:agc_canary_link_stub.c' 'libSceAgcDriver:agc_driver_canary_link_stub.c'; do
    library=${item%%:*}; file=${item#*:}
    "${c[@]}" -c "$stubs_src/$file" -o "$obj/$library.o"
    "$sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$out/stubs/$library.so" "$obj/$library.o"
done
objs=()
for src in frontend/fe_app frontend/fe_covers frontend/fe_renderer frontend/fe_text frontend/fe_vk frontend/fe_i18n frontend/fe_sound \
           catalog/xex catalog/json catalog/x360db catalog/fsutil catalog/titles catalog/install shelf/session ps5/shelf_ps5; do
    o="$obj/${src//\//_}.o"
    "${cxx[@]}" -c "$root/src/$src.cpp" -o "$o"
    objs+=("$o")
done

"$sdk/bin/prospero-lld" -T "$crt/rcomp_title.ld" --version-script "$crt/rcomp_title_symbols.map" \
    --exclude-libs=ALL --gc-sections --eh-frame-hdr --error-limit=0 -L "$sdk/target/lib" \
    --no-dynamic-linker -z nodynamic-undefined-weak -Map "$out/shelf.map" \
    -e _start -o "$out/shelf.pie.elf" "$obj/crt.o" "${objs[@]}" "$obj"/compat_*.o "$obj/limits.o" \
    "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" \
    "@$out/wraps.rsp" --whole-archive "$driver" --no-whole-archive \
    --start-group "$platform" "${runtime_libs[@]}" "$builtins" --end-group \
    --as-needed "$sdk/target/lib/libkernel.so" "$sdk/target/lib/libSceLibcInternal.so" "${sdk_libs[@]}" \
    > "$out/link.log" 2>&1 || { tail -60 "$out/link.log"; exit 1; }

no_wx() {
    python3 - "$1" <<'PY'
import pathlib,sys
rows=[line.split() for line in pathlib.Path(sys.argv[1]).read_text().splitlines() if line.lstrip().startswith('LOAD ')]
assert rows, 'missing LOAD segments'
assert all(not ('W' in ''.join(r[6:-1]) and 'E' in ''.join(r[6:-1])) for r in rows), 'W+X LOAD segment'
PY
}
llvm-readelf --program-headers --dynamic "$out/shelf.pie.elf" > "$out/readelf.txt"
no_wx "$out/readelf.txt"
if grep -q 'libkernel_web' "$out/readelf.txt"; then echo 'FAIL: payload module in the application' >&2; exit 1; fi
"$tool" link --in "$out/shelf.pie.elf" --out "$out/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$out/stubs/libSceAgc.so" --stub "$out/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > "$out/convert.log" 2>&1 ||
    { tail -60 "$out/convert.log"; exit 1; }
llvm-readelf --program-headers --dynamic "$out/eboot.elf" > "$out/converted-readelf.txt"
no_wx "$out/converted-readelf.txt"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$out/sign.log" 2>&1
python3 - "$root/sce_sys/param.json" "$app/sce_sys/param.json" "$title_id" <<'PY'
import json,sys
p=json.load(open(sys.argv[1],encoding='utf-8'))
tid=sys.argv[3]
p['titleId']=tid; p['conceptId']=tid[4:]; p['contentId']='UP9000-'+tid+'_00-RCOMPSHELF000000'
open(sys.argv[2],'w',encoding='utf-8',newline='\n').write(json.dumps(p,indent=2)+'\n')
PY
cp "$root/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
cp "$libc_prx" "$app/sce_module/libc.prx"
(cd "$app" && find . -type f ! -name manifest.sha256 -print0 | sort -z | xargs -0 sha256sum > manifest.sha256)
"$tool" self --inspect --file "$app/eboot.bin" > "$out/self.log"
printf 'PASS linked R-comp shelf %s build=%s; PS5 execution NOT TESTED\n' "$title_id" "$build_tag"
sha256sum "$app/eboot.bin"
