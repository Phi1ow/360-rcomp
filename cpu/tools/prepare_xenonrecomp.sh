#!/usr/bin/env bash
# Copies the pinned XenonRecomp submodule (with its submodules, without .git)
# to build/cpu-xenonrecomp-src and applies cpu/patches/xenonrecomp/*.patch in
# order. The submodule itself is never modified. Prints the patch hashes.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
src="$root/third_party/XenonRecomp"
dst="$root/build/cpu-xenonrecomp-src"
rm -rf "$dst" && mkdir -p "$dst"
(cd "$src" && tar --exclude=.git -cf - .) | (cd "$dst" && tar -xf -)
# Git for Windows checks the sources out with CRLF line endings (core.autocrlf=true, its default), which the patches do
# not match ("hunks FAILED"): the copy is normalised to LF. The submodule itself is never modified.
grep -rIl --null "$(printf '\r')" "$dst" | xargs -0 -r sed -i 's/\r$//' || true
for p in "$root"/cpu/patches/xenonrecomp/*.patch; do
    [[ -e $p ]] || continue
    patch -d "$dst" -p1 --quiet --forward < "$p"
    echo "applied $(basename "$p") sha256:$(sha256sum "$p" | cut -d' ' -f1)"
done
