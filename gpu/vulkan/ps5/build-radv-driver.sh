#!/usr/bin/env bash
# Build the one R-comp-pinned public RADV backend for PS5 from its isolated copy.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
base=${RCOMP_RADV_BUILD_ROOT:-$root/build/vulkan-gta-radv-20260928}
[[ $base == "$root"/build/* ]] || { echo 'build root must be under R-comp/build' >&2; exit 2; }
source_dir="$base/mesa-source"
build_dir="$base/radv-ps5"
sdk="$base/ps5-sdk"
pin=cedb774b27d089fa81f46add28d0a8c13ff0f7d2
[[ $(cat "$source_dir/.rcomp-source-commit") == "$pin" ]] || exit 2
[[ -f "$sdk/target/lib/libps5platform.a" ]] || { echo 'missing isolated platform runtime'; exit 2; }
export PATH="$base/mesa-host-clc/src/compiler/clc:$base/mesa-host-clc/src/compiler/spirv:$base/host-prefix/bin:/usr/bin:/bin"
export PYTHONPATH="$base/host-prefix/python${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONDONTWRITEBYTECODE=1
export LLVM_CONFIG=/usr/bin/llvm-config
command -v mesa_clc >/dev/null
command -v vtn_bindgen2 >/dev/null
printf "[constants]\nsdk = '%s'\n" "$sdk" > "$base/radv-cross-constants.ini"
if [[ ! -f $build_dir/build.ninja ]]; then
    meson setup "$build_dir" "$source_dir" \
      --cross-file "$base/radv-cross-constants.ini" \
      --cross-file "$base/src/PS5_Vulkan/tooling/radv/ps5-cross.ini" \
      -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dradv-winsys=ps5 \
      -Dllvm=disabled -Damd-use-llvm=false -Dvideo-codecs= \
      -Dbuildtype=debugoptimized -Db_ndebug=true \
      -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
      -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dzlib=enabled --force-fallback-for=zlib \
      -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=enabled -Dbuild-tests=false -Dvulkan-layers= -Dtools= \
      -Dmesa-clc=system -Dradv-build-id="$pin" > "$base/radv-ps5-configure.log" 2>&1 ||
        { tail -70 "$base/radv-ps5-configure.log"; exit 1; }
fi
ninja -C "$build_dir" -j6 src/amd/vulkan/libvulkan_radeon.a \
    > "$base/radv-ps5-build.log" 2>&1 || { tail -85 "$base/radv-ps5-build.log"; exit 1; }
sha256sum "$build_dir/src/amd/vulkan/libvulkan_radeon.a" > "$base/radv-ps5-archive.sha256"
cat "$base/radv-ps5-archive.sha256"
printf 'PASS compiled public RADV PS5 archive; title link and GPU execution NOT TESTED\n'
