#!/usr/bin/env bash
# R-comp's isolated build of the public pinned RADV shader-generation tools.
# Reference checkouts and the shared SDK are read-only inputs.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
base=${RCOMP_RADV_BUILD_ROOT:-$root/build/vulkan-gta-radv-20260928}
[[ $base == "$root"/build/* ]] || { echo 'build root must be under R-comp/build' >&2; exit 2; }
pin=cedb774b27d089fa81f46add28d0a8c13ff0f7d2
reference="$base/src/PS5_Mesa"
source_dir="$base/mesa-source"
build_dir="$base/mesa-host-clc"
export PATH="$base/host-prefix/bin:/usr/bin:/bin"
export PYTHONPATH="$base/host-prefix/python${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONDONTWRITEBYTECODE=1
export PKG_CONFIG_PATH="$base/host-prefix/lib/pkgconfig:/usr/lib/pkgconfig"
export LLVM_CONFIG=/usr/bin/llvm-config
export CC=clang CXX=clang++
for program in git tar clang clang++ llvm-config llvm-ar meson ninja pkg-config python3; do
    command -v "$program" >/dev/null || { echo "missing host program: $program" >&2; exit 2; }
done
[[ $(git -C "$reference" rev-parse HEAD) == "$pin" ]] ||
    { echo 'Mesa reference does not match the R-comp pin' >&2; exit 2; }
python3 - <<'PY'
import mako, yaml, packaging
print('Host Python:', mako.__version__, yaml.__version__, packaging.__version__)
PY
pkg-config --modversion LLVMSPIRVLib SPIRV-Tools
if [[ ! -d $source_dir ]]; then
    mkdir "$source_dir"
    git -C "$reference" archive --format=tar "$pin" | tar -xf - -C "$source_dir"
    printf '%s\n' "$pin" > "$source_dir/.rcomp-source-commit"
fi
[[ $(cat "$source_dir/.rcomp-source-commit") == "$pin" ]] ||
    { echo 'exported Mesa source identity missing/different' >&2; exit 2; }
for patch in "$root/gpu/vulkan/patches/mesa/"*.patch; do
    relative_source=${source_dir#"$root"/}
    if git -C "$root" apply --directory="$relative_source" --reverse --check "$patch" >/dev/null 2>&1; then
        continue
    fi
    git -C "$root" apply --directory="$relative_source" --check "$patch"
    git -C "$root" apply --directory="$relative_source" "$patch"
done
if [[ ! -f $build_dir/build.ninja ]]; then
    meson setup "$build_dir" "$source_dir" \
        -Dbuildtype=release -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
        -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled \
        -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
        -Dllvm=enabled -Dshared-llvm=enabled \
        -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled \
        -Dzstd=disabled -Dxmlconfig=disabled -Dtools= \
        > "$base/mesa-host-configure.log" 2>&1 ||
        { tail -70 "$base/mesa-host-configure.log"; exit 1; }
fi
ninja -C "$build_dir" -j4 src/compiler/clc/mesa_clc.exe src/compiler/spirv/vtn_bindgen2.exe \
    > "$base/mesa-host-build.log" 2>&1 ||
    { tail -75 "$base/mesa-host-build.log"; exit 1; }
sha256sum "$build_dir/src/compiler/clc/mesa_clc.exe" \
    "$build_dir/src/compiler/spirv/vtn_bindgen2.exe" > "$base/mesa-host-tools.sha256"
cat "$base/mesa-host-tools.sha256"
printf 'PASS real host shader-generation tools; GPU execution NOT TESTED\n'
