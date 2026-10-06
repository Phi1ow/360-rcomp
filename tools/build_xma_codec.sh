#!/usr/bin/env bash
# Build only the independent FFmpeg XMA frame codec, never the rexglue audio runtime.
set -euo pipefail
mode=${1:?usage: build_xma_codec.sh host|ps5 source-directory output-directory [ps5-sdk]}
source_dir=$(cd -- "${2:?source-directory required}" && pwd)
out=${3:?output-directory required}
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)
prefix="$out/prefix"
test -f "$source_dir/libavcodec/wmaprodec.c"
test ! -e "$source_dir/config.h" || { echo 'FAIL: source/config.h prevents an out-of-tree build; use an unconfigured private source copy' >&2; exit 1; }
mkdir -p -- "$out/obj"
common=(--prefix="$prefix" --disable-autodetect --disable-everything
  --enable-decoder=xmaframes --disable-programs --disable-doc --disable-network
  --disable-avdevice --disable-avfilter --disable-avformat --disable-postproc
  --disable-swresample --disable-swscale --disable-asm --disable-x86asm
  --disable-debug --enable-static --disable-shared --disable-pthreads
  --disable-w32threads --disable-os2threads --disable-bzlib --disable-iconv
  --disable-lzma --disable-zlib --extra-cflags='-march=x86-64-v3 -ffp-contract=on -fPIC')
case "$mode" in
  host) target=(--cc=clang --ar=llvm-ar --ranlib=llvm-ranlib) ;;
  ps5)
    sdk=$(cd -- "${4:?ps5-sdk required}" && pwd)
    target=(--enable-cross-compile --target-os=freebsd --arch=x86_64
      --cc="$sdk/bin/prospero-clang" --ar=llvm-ar --ranlib=llvm-ranlib)
    ;;
  *) echo "FAIL: unknown target $mode" >&2; exit 1 ;;
esac
cd -- "$out/obj"
bash "$source_dir/configure" "${common[@]}" "${target[@]}" > "$out/configure.log" 2>&1
# The selected decoder pulls only wmapro and the required DSP functions.
grep -q '^#define CONFIG_XMAFRAMES_DECODER 1$' config.h
grep -q '^#define CONFIG_GPL 0$' config.h
grep -q '^#define CONFIG_NONFREE 0$' config.h
make -j"${RCOMP_CODEC_JOBS:-8}" > "$out/build.log" 2>&1
make install > "$out/install.log" 2>&1
mkdir -p -- "$prefix/share/rcomp-xma-codec"
cp -- "$source_dir/COPYING.LGPLv2.1" "$source_dir/LICENSE.md" config.h ffbuild/config.mak "$prefix/share/rcomp-xma-codec/"
sha256sum "$prefix/lib/libavcodec.a" "$prefix/lib/libavutil.a" > "$out/libraries.sha256"
printf 'PASS XMA frame codec compiled: %s (%s); execution NOT TESTED\n' "$prefix" "$mode"
