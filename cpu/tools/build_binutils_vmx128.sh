#!/usr/bin/env bash
# Host assembler with Xenon VMX128 support: GNU binutils 2.24 + Xenia's
# binutils-2.24-vmx128.patch (xenia @95a5c3ee, BSD-3 project; binutils GPL-3).
# Built only for the host into build/cpu-binutils-vmx128 (never shipped);
# the pinned tarball hash and patch hash are checked before building.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
deps="${RCOMP_DEPS:-$(dirname "$root")/deps}"
out="$root/build/cpu-binutils-vmx128"
tarball_url=https://mirrors.kernel.org/sourceware/binutils/releases/binutils-2.24.tar.gz
tarball_sha=4930b2886309112c00a279483eaef2f0f8e1b1b62010e0239c16b22af7c346d4
patch="$deps/xenia/third_party/binutils/binutils-2.24-vmx128.patch"
patch_sha=$(sha256sum "$patch" | cut -d' ' -f1)
[[ -x $out/bin/powerpc-none-elf-as ]] && { echo "ok (cached) $out/bin"; exit 0; }
mkdir -p "$out/src"
cd "$out/src"
[[ -f binutils-2.24.tar.gz ]] || curl -fsSL -o binutils-2.24.tar.gz "$tarball_url"
echo "$tarball_sha  binutils-2.24.tar.gz" | sha256sum --check --strict --quiet
rm -rf binutils-2.24 && tar xzf binutils-2.24.tar.gz
cd binutils-2.24
patch -p0 --quiet < "$patch"
# Old sources vs GCC 13: tolerate common symbols and old C idioms.
CFLAGS="-O2 -fcommon -std=gnu89 -Wno-error" ./configure --disable-werror --disable-nls \
    --disable-gdb --disable-sim --disable-gprof --disable-ld --target=powerpc-none-elf \
    --prefix="$out" >"$out/configure.log" 2>&1
make -j"$(nproc)" MAKEINFO=true all-gas all-binutils >"$out/make.log" 2>&1
make MAKEINFO=true install-gas install-binutils >"$out/install.log" 2>&1
printf 'tarball %s\npatch %s\n' "$tarball_sha" "$patch_sha" > "$out/PROVENANCE"
"$out/bin/powerpc-none-elf-as" --version | head -1
