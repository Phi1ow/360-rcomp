#!/usr/bin/env bash
# Builds the PS5 test kit: one ready-to-copy title folder per test, all with
# title id PPSA88360 (run them one at a time), plus SHA256SUMS and the
# expected results. Nothing here touches a console.
#
#   RCOMP_ACCEPT_GPL_TITLE_TOOLS=1 EMU3=/path/to/emu-3 tools/ps5_test_kit.sh
#
# The ELF->SELF conversion, fake signing and sce_module/libc.prx come from
# PS5_RetroArch's GPL-3.0 host tools (platform/ps5/tools/build_title.sh);
# without the opt-in the kit stops with BLOCKED.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
b="$root/build"
kit="$b/ps5-kit"
step() { echo "==> [kit] $*" >&2; }
[[ ${RCOMP_ACCEPT_GPL_TITLE_TOOLS:-0} == 1 ]] || {
    echo "BLOCKED: set RCOMP_ACCEPT_GPL_TITLE_TOOLS=1 (GPL-3.0 title tools, see docs/STATUS.md)" >&2; exit 3; }
xr="$b/cpu-xenonrecomp/XenonRecomp/XenonRecomp"
cfg=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/ps5.cmake" -DRCOMP_ROOT="$root")

step "CPU corpus (host run + PS5 archives)"
"$root/tools/cpu_pipeline.sh" >"$b/kit-cpu-pipeline.log" 2>&1

for s in m3 m5; do
    step "$s corpus + PS5 archives"
    python3 "$root/cpu/tools/build_corpus.py" --set "$s=$root/fixtures/ppc/$s" --out "$b/$s-corpus" \
        --xenonrecomp "$xr" --xenonrecomp-src "$root/third_party/XenonRecomp" \
        --patches "$root/cpu/patches/xenonrecomp" >/dev/null
    python3 "$root/tests/cpu/gen_harness.py" --corpus "$b/$s-corpus" --out "$b/$s-cases" >/dev/null
done
cmake -S "$root/tests/m3" -B "$b/tests-m3-ps5" "${cfg[@]}" -DCORPUS_DIR="$b/m3-corpus" \
    -DCASES_DIR="$b/m3-cases" >"$b/kit-m3.cfg.log"
ninja -C "$b/tests-m3-ps5" rcomp_m3_selftest >"$b/kit-m3.build.log"

step "Xenos command processor (rexglue) title"
"$root/gpu/xenos/rexglue/prepare.sh" >"$b/kit-xenos-prepare.log" 2>&1
cmake -S "$root/tests/xenos_cp" -B "$b/tests-xenos-cp-ps5" "${cfg[@]}" >"$b/kit-xenos-cp.cfg.log"
ninja -C "$b/tests-xenos-cp-ps5" rcomp_xenos_cp_selftest rcomp_xenos_video_selftest rcomp_xenos_vulkan_selftest \
    OGLCompiler OSDependent >"$b/kit-xenos-cp.build.log"
"$root/platform/ps5/tools/build_title.sh" --xenos-cp >"$b/kit-title-xenos-cp.log" 2>&1
cmake -S "$root/tests/xenos_cp" -B "$b/tests-xenos-cp" -G Ninja -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ -DRCOMP_ROOT="$root" >"$b/kit-xenos-cp-host.cfg.log"
ninja -C "$b/tests-xenos-cp" rcomp_xenos_cp_tests rcomp_xenos_video_tests rcomp_xenos_vulkan_tests >"$b/kit-xenos-cp-host.build.log"

step "game shell + recompiled graphics XEX (PS5 archives)"
cmake -S "$root/tests/xex_gfx" -B "$b/tests-xex-gfx-ps5" "${cfg[@]}" -DXEX_GEN_DIR="$b/xex-gfx/ppc" \
    -DRCOMP_XEX=/app0/fixtures/xex/rcomp_gfx_title.xex >"$b/kit-xex-gfx.cfg.log"
ninja -C "$b/tests-xex-gfx-ps5" rcomp_xex_gfx_selftest rcomp_platform_input OGLCompiler OSDependent \
    >"$b/kit-xex-gfx.build.log"
cmake -S "$root/tests/xex_gfx" -B "$b/tests-xex-gfx-host" -G Ninja -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ -DRCOMP_ROOT="$root" -DXEX_GEN_DIR="$b/xex-gfx/ppc" \
    -DRCOMP_XEX="$b/xex-gfx/rcomp_gfx_title.xex" >"$b/kit-xex-gfx-host.cfg.log"
ninja -C "$b/tests-xex-gfx-host" rcomp_xex_gfx_tests >"$b/kit-xex-gfx-host.build.log"

step "Vulkan: PS5_Vulkan fork from Emu-3 (driver, probe, draw test, M5)"
"$root/gpu/vulkan/ps5/build-emu3-fork.sh" >"$b/kit-emu3-fork.log" 2>&1  # driver + snapshot (+ first link)
vki="$b/vulkan-emu3-artifacts/driver/.deps/work/psbc-ps5/third_party/Vulkan-Headers/include"
cmake -S "$root/tests/m5" -B "$b/tests-m5-ps5" "${cfg[@]}" -DCORPUS_DIR="$b/m5-corpus" \
    -DCASES_DIR="$b/m5-cases" -DRCOMP_VK_INCLUDE="$vki" >"$b/kit-m5.cfg.log"
ninja -C "$b/tests-m5-ps5" rcomp_m5_selftest >"$b/kit-m5.build.log"
src="$b/vulkan-emu3-driver/PS5_Vulkan"
z="$src/.deps/native/zlib/root/usr"
rm -rf "$b/vulkan-emu3-out"
RCOMP_M5_BUILD="$b/tests-m5-ps5" RCOMP_XENOS_VK_BUILD="$b/tests-xenos-cp-ps5" RCOMP_XEX_GFX_BUILD="$b/tests-xex-gfx-ps5" \
    RCOMP_VK_LINK_CONTRACT=ps5vk-driver ZLIB_INCLUDE="$z/include" \
    ZLIB_ARCHIVE="$z/lib/libz.a" bash "$root/gpu/vulkan/ps5/build-ps5.sh" "$b/vulkan-emu3-artifacts" \
    "$src/.deps/native/ps5-payload-sdk" "$b/vulkan-emu3-out" >"$b/kit-vk-link.log" 2>&1

step "platform titles (platform self-test, CPU corpus, M3)"
"$root/platform/ps5/tools/build_title.sh" >"$b/kit-title.log" 2>&1
"$root/platform/ps5/tools/build_title.sh" --cpu-corpus >"$b/kit-title-cpu.log" 2>&1
"$root/platform/ps5/tools/build_title.sh" --m3 >"$b/kit-title-m3.log" 2>&1
# Synthetic XEX (host run done by cpu_pipeline.sh, which also regenerated build/xex).
cmake -S "$root/tests/xex" -B "$b/tests-xex-ps5" "${cfg[@]}" -DXEX_GEN_DIR="$b/xex/ppc" \
    -DRCOMP_XEX=/app0/fixtures/xex/rcomp_title.xex >"$b/kit-xex.cfg.log"
ninja -C "$b/tests-xex-ps5" rcomp_xex_selftest >"$b/kit-xex.build.log"
"$root/platform/ps5/tools/build_title.sh" --xex >"$b/kit-title-xex.log" 2>&1

step "assemble $kit"
id=PPSA88360
rm -rf "$kit" && mkdir -p "$kit"
put_title() {  # name dist_dir
    mkdir -p "$kit/$1" && cp -a "$2/$id" "$kit/$1/$id"
}
put_vk() {     # name eboot [file dest-in-title]...
    mkdir -p "$kit/$1" && cp -a "$b/platform-ps5-title/dist/$id" "$kit/$1/$id"
    cp "$2" "$kit/$1/$id/eboot.bin"
    local dir="$kit/$1/$id"
    shift 2
    while (($#)); do mkdir -p "$dir/$(dirname "$2")" && cp "$1" "$dir/$2"; shift 2; done
    (cd "$kit/$1/$id" && find . -type f ! -name manifest.sha256 -print0 | sort -z |
        xargs -0 sha256sum >manifest.sha256)
}
put_title 1-platform "$b/platform-ps5-title/dist"
put_title 2-cpu-corpus "$b/platform-ps5-title-cpu/dist"
put_title 3-m3-program "$b/platform-ps5-title-m3/dist"
put_vk 4-vk-probe "$b/vulkan-emu3-out/rcomp_vk_probe/eboot.bin"
put_vk 5-vk-probe-experimental "$b/vulkan-emu3-out/rcomp_vk_probe_exp/eboot.bin"
put_vk 6-vk-draw-test "$b/vulkan-emu3-out/rcomp_vk_draw_test/eboot.bin"
put_vk 7-m5-graphics "$b/vulkan-emu3-out/rcomp_m5/eboot.bin"
put_title 8-xex-title "$b/platform-ps5-title-xex/dist"
put_title 9-xenos-cp "$b/platform-ps5-title-xenos-cp/dist"
put_vk 10-xenos-vk "$b/vulkan-emu3-out/rcomp_xenos_vk/eboot.bin"
put_vk 11-xex-gfx "$b/vulkan-emu3-out/rcomp_xex_gfx/eboot.bin" \
    "$b/xex-gfx/rcomp_gfx_title.xex" fixtures/xex/rcomp_gfx_title.xex
cp "$root/docs/PS5_TEST_KIT.md" "$kit/README.md"
mkdir -p "$kit/expected" "$kit/tools"
"$b/tests-cpu-host/rcomp_cpu_tests" > "$kit/expected/cpu-host.jsonl" 2>/dev/null
"$b/tests-m3-host/rcomp_m3_tests" > "$kit/expected/m3-host.jsonl" 2>/dev/null
"$b/tests-xex-host/rcomp_xex_tests" > "$kit/expected/xex-host.txt" 2>/dev/null
"$b/tests-xenos-cp/rcomp_xenos_cp_tests" > "$kit/expected/xenos-cp-host.txt" 2>/dev/null
"$b/tests-xenos-cp/rcomp_xenos_video_tests" >> "$kit/expected/xenos-cp-host.txt" 2>/dev/null
"$b/tests-xenos-cp/rcomp_xenos_vulkan_tests" > "$kit/expected/xenos-vk-host.txt" 2>/dev/null || true
"$b/tests-xex-gfx-host/rcomp_xex_gfx_tests" > "$kit/expected/xex-gfx-host.txt" 2>/dev/null || true
cp "$b/vulkan-host/probe-emu3-default.json" "$kit/expected/probe-hostmodel-default.json" 2>/dev/null || true
cp "$b/vulkan-host/probe-emu3-experimental.json" "$kit/expected/probe-hostmodel-experimental.json" 2>/dev/null || true
cp "$root/platform/ps5/tools/collect_cpu_results.py" "$root/tools/compare_cpu_results.py" "$kit/tools/"
# Licence texts and notices of what the binaries contain (GPL-3.0 components:
# PS5_Vulkan fork, PS5_RetroArch title tooling and libc.prx).
lic="$kit/licenses"
mkdir -p "$lic"
cp "$b/vulkan-emu3-driver/PS5_Vulkan/LICENSE" "$lic/GPL-3.0.txt"
cp "$b/vulkan-emu3-driver/PS5_Vulkan/NOTICE.md" "$lic/PS5_Vulkan-NOTICE.md"
cp "$root/third_party/XenonRecomp/LICENSE.md" "$lic/XenonRecomp-MIT.md"
cp "$root/third_party/XenonRecomp/thirdparty/simde/COPYING" "$lic/simde-MIT.txt"
# Test 9: rexglue GPU subsystem (Xenia-derived) and what it links.
xt="$b/xenos-rexglue-thirdparty"
cp "$root/gpu/xenos/rexglue/LICENSE.rexglue" "$lic/rexglue-sdk-BSD-3.txt"
cp "$xt/fmt/LICENSE" "$lic/fmt-MIT.txt"
cp "$xt/spdlog/LICENSE" "$lic/spdlog-MIT.txt"
cp "$xt/glslang/LICENSE.txt" "$lic/glslang.txt"
cp "$xt/spirv-headers/LICENSE" "$lic/SPIRV-Headers.txt"
cp "$xt/xxHash/LICENSE" "$lic/xxHash-BSD-2.txt"
cp "$xt/utfcpp/LICENSE" "$lic/utfcpp-BSL-1.0.txt"
cp "$xt/vulkan-memory-allocator/LICENSE.txt" "$lic/VulkanMemoryAllocator-MIT.txt"
cat >"$lic/NOTICES.md" <<NOTICES
# Kit binary contents and licences

| Component | Version | Licence | Where | Source |
| --- | --- | --- | --- | --- |
| R-comp (project code) | $(git -C "$root" rev-parse --short HEAD) | no licence chosen yet (docs/STATUS.md) | all eboots | github.com/Phi1ow/r-comp |
| PS5_Vulkan (Emu-3 fork) | 17350536 | GPL-3.0-or-later (GPL-3.0.txt) | tests 4-7, 10-11 | Phi1ow/Emu-3, vulkan11-reprise/bundles/PS5_Vulkan.bundle |
| PS5_RetroArch tooling/native (CRT, ELF->SELF conversion, libc.prx) | 18dc1059 | GPL-3.0-or-later (GPL-3.0.txt) | all titles | github.com/mihawk-99/PS5_RetroArch |
| XenonRecomp (generator; ppc_context.h prelude in the generated code) | ddd128bc + cpu/patches/xenonrecomp | MIT (XenonRecomp-MIT.md) | tests 2, 3, 7, 8, 11 | github.com/hedge-dev/XenonRecomp |
| SIMDe (headers used by the generated code) | a532a12c | MIT (simde-MIT.txt) | tests 2, 3, 7, 8, 11 | github.com/simd-everywhere/simde-no-tests |
| libc++, libc++abi, libunwind (statically linked) | ps5-payload-sdk v0.42 | Apache-2.0 WITH LLVM-exception | tests 3-11 | github.com/ps5-payload-dev/sdk |
| rexglue-sdk, GPU subsystem (Xenia-derived) + gpu/xenos/rexglue/patches | c94f5ebd | BSD-3-Clause (rexglue-sdk-BSD-3.txt) | tests 9-11 | github.com/rexglue/rexglue-sdk |
| fmt, spdlog | pinned by rexglue (deps/deps.lock) | MIT (fmt-MIT.txt, spdlog-MIT.txt) | tests 9-11 | github.com/fmtlib/fmt, github.com/gabime/spdlog |
| glslang (SpvBuilder; full GLSL compiler in tests 10-11), SPIRV-Headers | pinned by rexglue | glslang.txt, SPIRV-Headers.txt | tests 9-11 | github.com/KhronosGroup |
| xxHash, utfcpp | pinned by rexglue | BSD-2-Clause, BSL-1.0 | tests 9-11 |
| Vulkan Memory Allocator | pinned by rexglue | MIT (VulkanMemoryAllocator-MIT.txt) | tests 10-11 | github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | github.com/Cyan4973/xxHash, github.com/nemtrif/utfcpp |

The GPL-3.0 components make every eboot that contains them GPL-3.0 as a
whole. No game file, key or proprietary SDK is included.
NOTICES
(cd "$kit" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS)
git -C "$root" rev-parse HEAD > "$kit/SOURCE_COMMIT"
git -C "$root" status --porcelain | wc -l | sed 's/^/dirty_files=/' >> "$kit/SOURCE_COMMIT"
(cd "$b" && rm -f ps5-kit.tar.gz && tar czf ps5-kit.tar.gz ps5-kit)
ls -la "$b/ps5-kit.tar.gz"
