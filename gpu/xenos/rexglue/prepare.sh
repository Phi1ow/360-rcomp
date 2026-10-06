#!/usr/bin/env bash
# Copies the pinned rexglue-sdk GPU sources into build/xenos-rexglue-src,
# fetches the submodules the GPU needs at rexglue's gitlink SHAs, and applies
# gpu/xenos/rexglue/patches/*.patch in order. The checkout in $RCOMP_DEPS is
# only read. Prints what it applied.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
deps="${RCOMP_DEPS:-$(dirname "$root")/deps}"
src="$deps/rexglue-sdk"
dst="$root/build/xenos-rexglue-src"
pin=c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
[[ $(git -C "$src" rev-parse HEAD) == "$pin" ]] || { echo "BLOCKED: $src is not at $pin" >&2; exit 2; }
git -C "$src" diff --quiet || { echo "BLOCKED: $src has local modifications" >&2; exit 2; }

# name|url|sha|what is kept
subs=(
  "glslang|https://github.com/KhronosGroup/glslang.git|f4f1d8a352ca1908943aea2ad8c54b39b4879080"
  "spirv-headers|https://github.com/KhronosGroup/SPIRV-Headers.git|29981f65241605e08b0ede4cfeb999fe3b723c6a"
  "vulkan-headers|https://github.com/KhronosGroup/Vulkan-Headers.git|e3b1eec08173d6b825cd3ac88c885a63b621504a"
  "vulkan-memory-allocator|https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git|1d8f600fd424278486eade7ed3e877c99f0846b1"
  "xxHash|https://github.com/Cyan4973/xxHash.git|e626a72bc2321cd320e953a0ccf1584cad60f363"
  "fmt|https://github.com/fmtlib/fmt.git|407c905e45ad75fc29bf0f9bb7c5c2fd3475976f"
  "spdlog|https://github.com/gabime/spdlog.git|79524ddd08a4ec981b7fea76afd08ee05f83755d"
  "utfcpp|https://github.com/nemtrif/utfcpp.git|63d64de49fd6b829f7c8694df5ab2ee625cb7134"
)
cache="$root/build/xenos-rexglue-thirdparty"
mkdir -p "$cache"
for s in "${subs[@]}"; do
  IFS='|' read -r name url sha <<<"$s"
  d="$cache/$name"
  if [[ ! -d $d/.git ]] || [[ $(git -C "$d" rev-parse HEAD 2>/dev/null) != "$sha" ]]; then
    rm -rf "$d" && git init -q "$d"
    git -C "$d" fetch -q --depth 1 "$url" "$sha"
    git -C "$d" checkout -q FETCH_HEAD
  fi
  [[ $(git -C "$d" rev-parse HEAD) == "$sha" ]] || { echo "FAIL: $name not at $sha" >&2; exit 1; }
done

rm -rf "$dst" && mkdir -p "$dst/thirdparty"
(cd "$src" && tar --exclude=.git --exclude='src/graphics/d3d12' -cf - LICENSE include src thirdparty/renderdoc) | (cd "$dst" && tar -xf -)
for s in "${subs[@]}"; do
  IFS='|' read -r name _ _ <<<"$s"
  (cd "$cache/$name" && tar --exclude=.git -cf - .) | (mkdir -p "$dst/thirdparty/$name" && cd "$dst/thirdparty/$name" && tar -xf -)
done
for p in "$root"/gpu/xenos/rexglue/patches/*.patch; do
  [[ -e $p ]] || continue
  if [[ $(basename "$p") == 0012-reuse-texture-descriptors.patch && ${RCOMP_XENOS_DESCRIPTOR_REUSE:-0} != 1 ]]; then
    echo "skipped opt-in performance candidate $(basename "$p")"
    continue
  fi
  patch -d "$dst" -p1 --quiet --forward <"$p"
  echo "applied $(basename "$p") sha256:$(sha256sum "$p" | cut -d' ' -f1)"
done
echo "rexglue-sdk@${pin:0:12} GPU sources ready in $dst"
