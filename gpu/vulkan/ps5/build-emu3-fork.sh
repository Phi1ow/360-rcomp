#!/usr/bin/env bash
# Builds the probe and the draw test against the PS5_Vulkan fork carried by the
# owner's private repository Phi1ow/Emu-3 (read only):
#   bundle vulkan11-reprise/bundles/PS5_Vulkan.bundle (sha256 pinned below),
#   commit 17350536acf9ea1a3f747e0a20b3521e42e41d9e.
# Everything is built in build/vulkan-emu3-*; Emu-3 and $RCOMP_DEPS are never
# written. Execution on a console is not part of this script.
#
#   EMU3=/path/to/emu-3 gpu/vulkan/ps5/build-emu3-fork.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
emu3=${EMU3:-/home/user/emu-3}
bundle="$emu3/vulkan11-reprise/bundles/PS5_Vulkan.bundle"
bundle_sha=4401390f33c054302b6eb39427037e0d662d7533d4e29465c01050d9f9c47e22
commit=17350536acf9ea1a3f747e0a20b3521e42e41d9e
b="$root/build"
src="$b/vulkan-emu3-driver/PS5_Vulkan"
echo "$bundle_sha  $bundle" | sha256sum --check --strict --quiet
if [[ ! -d $src/.git ]]; then
    mkdir -p "$b/vulkan-emu3-driver"
    git clone -q "$bundle" "$src"
fi
git -C "$src" checkout -q "$commit"
[[ $(git -C "$src" rev-parse HEAD) == "$commit" ]]
[[ -z $(git -C "$src" status --porcelain) ]] || { echo "FAIL: $src is dirty" >&2; exit 1; }
# The driver's own dependency and build steps (Makefile target `driver`).
(cd "$src" && make driver) >"$b/vulkan-emu3-driver/log-make-driver.txt" 2>&1
sdk="$src/.deps/native/ps5-payload-sdk"
bash "$root/gpu/vulkan/ps5/build-mesa-util.sh" "$src" "$sdk" >/dev/null
python3 "$root/gpu/vulkan/snapshot_manifest.py" --source "$src" --dest "$b/vulkan-emu3-artifacts/driver" \
    --profile driver --path build/rcomp-mesa-util --label "emu3-${commit:0:8}" --commit-from "$src" --force
# PS5_RetroArch link inputs: same pinned snapshot as the public build.
[[ -d $b/vulkan-emu3-artifacts/retroarch-link ]] ||
    cp -a "$b/vulkan-ps5-artifacts/retroarch-link" "$b/vulkan-emu3-artifacts/retroarch-link"
z="$src/.deps/native/zlib/root/usr"
rm -rf "$b/vulkan-emu3-out"
RCOMP_VK_LINK_CONTRACT=ps5vk-driver ZLIB_INCLUDE="$z/include" ZLIB_ARCHIVE="$z/lib/libz.a" \
    bash "$root/gpu/vulkan/ps5/build-ps5.sh" "$b/vulkan-emu3-artifacts" "$sdk" "$b/vulkan-emu3-out"
