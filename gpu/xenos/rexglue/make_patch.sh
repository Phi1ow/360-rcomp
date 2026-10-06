#!/usr/bin/env bash
# Writes the changes made in build/xenos-rexglue-src on top of the existing
# patches as a new patch: gpu/xenos/rexglue/make_patch.sh NNNN-name
# (compares against a pristine copy + patches/*.patch; only include/ and src/).
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
name=${1:?usage: make_patch.sh NNNN-name}
b="$root/build"
base="$b/xenos-rexglue-base"
rm -rf "$base" && mkdir -p "$base"
deps="${RCOMP_DEPS:-$(dirname "$root")/deps}"
(cd "$deps/rexglue-sdk" && tar --exclude=.git -cf - include src) | (cd "$base" && tar -xf -)
for p in "$root"/gpu/xenos/rexglue/patches/*.patch; do
  [[ -e $p ]] && patch -d "$base" -p1 --quiet --forward <"$p"
done
out="$root/gpu/xenos/rexglue/patches/$name.patch"
(cd "$b" && diff -ruN -x d3d12 xenos-rexglue-base/include xenos-rexglue-src/include;
 cd "$b" && diff -ruN -x d3d12 xenos-rexglue-base/src xenos-rexglue-src/src) |
  sed 's|^diff -ruN -x d3d12 xenos-rexglue-base/\([^ ]*\) xenos-rexglue-src/.*|diff -ruN a/\1 b/\1|; s|^--- xenos-rexglue-base/|--- a/|; s|^+++ xenos-rexglue-src/|+++ b/|' >"$out" || true
[[ -s $out ]] || { rm -f "$out"; echo "no change" >&2; exit 1; }
echo "wrote $out ($(grep -c '^@@' "$out") hunks)"
