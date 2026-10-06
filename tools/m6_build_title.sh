#!/usr/bin/env bash
# Local XEX -> linked PS5 artifacts -> optional complete local title folder.
# RCOMP_M6_GAME_ROOT and RCOMP_M6_TITLE_TEMPLATE enable assembly after linking.
# No deployment; a linked eboot alone is not a bootable distribution.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
xex=$(realpath -- "${1:?usage: tools/m6_build_title.sh XEX ART SDK [NAME]}")
art=$(realpath -- "${2:?ART snapshot directory required}")
sdk=$(realpath -- "${3:?PS5 SDK directory required}")
name=${4:-$(basename "$xex" .xex)}
[[ $name =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo "BLOCKED: unsafe title name" >&2; exit 2; }
[[ -f $xex ]] || { echo "BLOCKED: XEX file not found" >&2; exit 2; }
case "$xex" in "$root"/*) echo "BLOCKED: XEX must be outside the repository" >&2; exit 2;; esac
mkdir -p "$root/build/m6"
base=$(realpath -- "$root/build/m6")
[[ $base == "$root/build/m6" ]] || { echo "BLOCKED: build/m6 must not redirect through a symlink" >&2; exit 2; }
# Neither generated code nor cached C++ objects from an older XEX are reused.
run=$(mktemp -d "$base/$name.XXXXXXXX")
work="$run/inventory"
ps5_build="$run/ps5"
dist="$run/linked"
printf 'Local work directory: %s\n' "$run"
python3 "$root/tools/m6_inventory.py" "$xex" --out "$work" --require-supported
python3 "$root/tools/m6_boot_gate.py" "$work" --xex "$xex"
# CPU setup alone does not prepare the GPU sources. Do not run another
# shared xenos build/prepare concurrently with this step.
"$root/gpu/xenos/rexglue/prepare.sh"
cmake -S "$root/app/m6" -B "$ps5_build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/ps5.cmake" -DRCOMP_PS5_SDK="$sdk" \
  -DRCOMP_ROOT="$root" -DM6_WORK="$work" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ps5_build" --target rcomp_m6_title rcomp_m6_generated
RCOMP_M6_BUILD="$ps5_build" RCOMP_M6_XEX="$work/plain.xex" \
RCOMP_VK_LINK_CONTRACT=ps5vk-driver "$root/gpu/vulkan/ps5/build-ps5.sh" "$art" "$sdk" "$dist"
printf 'INTERMEDIATE_ARTIFACTS_BUILT: %s/rcomp_m6 (local game-derived data; do not publish)\n' "$dist"
if [[ -z ${RCOMP_M6_GAME_ROOT:-} || -z ${RCOMP_M6_TITLE_TEMPLATE:-} ]]; then
    printf 'BLOCKED: linking finished; set RCOMP_M6_GAME_ROOT and RCOMP_M6_TITLE_TEMPLATE for title assembly. PS5/game boot NOT TESTED.\n' >&2
    exit 3
fi
python3 "$root/tools/m6_package_title.py" \
    --linked "$dist/rcomp_m6" --image "$work/plain.xex" \
    --game-root "$RCOMP_M6_GAME_ROOT" --template "$RCOMP_M6_TITLE_TEMPLATE" \
    --out "$run/title"
printf 'PASS: title folder assembled at %s/title; executable validity, PS5 execution and GTA IV NOT TESTED.\n' "$run"
