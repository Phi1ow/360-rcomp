#!/usr/bin/env bash
# Original R-comp probe. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
out=${1:-$root/build/vulkan-depth-msaa-transfer-20260930}
[[ $out == "$root"/build/vulkan-* ]] || { echo 'BLOCKED output must be Agent4 build/vulkan-*' >&2; exit 2; }
export PATH=/usr/bin:/bin
mkdir -p "$out/shaders"
command -v glslangValidator >/dev/null || { echo 'BLOCKED glslangValidator unavailable' >&2; exit 2; }
command -v spirv-val >/dev/null || { echo 'BLOCKED spirv-val unavailable' >&2; exit 2; }
header="$out/shaders/depth_probe_spirv.h"
printf '/* Generated from original probe GLSL; do not edit. */\n#include <stdint.h>\n' > "$header"
glslangValidator --version > "$out/shaders/tool-version.txt"
: > "$out/shaders/SHADERS.lock"
for name in quad.vert fill.frag transfer.frag snapshot.comp; do
    glslangValidator -V --target-env vulkan1.0 -o "$out/shaders/$name.spv" "$here/shaders/$name"
    spirv-val --target-env vulkan1.0 "$out/shaders/$name.spv"
    python3 - "$out/shaders/$name.spv" "${name/./_}_spv" >> "$header" <<'PY'
import pathlib,struct,sys
b=pathlib.Path(sys.argv[1]).read_bytes()
w=struct.unpack('<%dI'%(len(b)//4),b)
assert w[1]==0x10000
print('static const uint32_t %s[] = {'%sys.argv[2])
for i in range(0,len(w),8):print('  '+','.join('0x%08Xu'%x for x in w[i:i+8])+',')
print('};')
PY
    sha256sum "$here/shaders/$name" "$out/shaders/$name.spv" >> "$out/shaders/SHADERS.lock"
done
clang -std=c11 -O2 -Wall -Wextra -Werror "$here/check-oracle.c" -o "$out/check-oracle.exe"
"$out/check-oracle.exe" > "$out/host-oracle.log"
# Reuse R-comp's exact native title link contract, in an isolated generated
# script. The active common builder and the SDK/driver references are untouched.
python3 - "$root" "$out" "$here" <<'PY'
from pathlib import Path
import hashlib,sys
root,out,here=map(Path,sys.argv[1:])
src=root/'gpu/vulkan/ps5/build-radv-probes.sh'
s=src.read_text()
changes={
 'root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)':'root=${RCOMP_DEPTH_ROOT:?}',
 '-I"$root/gpu/vulkan/draw_test/generated"':'-I"$out/shaders"',
 'else source_file="$root/gpu/vulkan/draw_test/draw_test.c";defs=();fi':'else source_file="$RCOMP_DEPTH_SOURCE/depth_probe.c";defs=();fi',
}
for a,b in changes.items():
    assert s.count(a)==1,a
    s=s.replace(a,b)
(out/'native-link.sh').write_text(s)
(out/'builder-reference.sha256').write_text(hashlib.sha256(src.read_bytes()).hexdigest()+' '+str(src)+'\n')
PY
sdk="$root/build/vulkan-gta-radv-20260928/ps5-sdk"
headers="$root/build/prime-vulkan-headers/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include"
"$sdk/bin/prospero-clang" -std=gnu11 -O2 -Wall -Wextra -Werror -fsyntax-only -DRCOMP_TARGET_PS5=1 -I"$headers" -I"$root/gpu/vulkan/common" -I"$out/shaders" "$here/depth_probe.c"
RCOMP_DEPTH_ROOT="$root" RCOMP_DEPTH_SOURCE="$here" RCOMP_RADV_PROBES_OUT="$out" bash "$out/native-link.sh" draw
sha256sum "$here/depth_probe.c" "$here/oracle.h" "$here/check-oracle.c" "$header" > "$out/source-inputs.sha256"
cat "$out/host-oracle.log"
printf 'PASS probe/shaders/oracle build; PS5 execution NOT TESTED\n'
