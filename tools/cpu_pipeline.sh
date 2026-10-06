#!/usr/bin/env bash
# End-to-end CPU proof chain (M1 host, M2 PS5 compile):
#   PPC fixtures -> GNU as -> XenonRecomp (real, pinned) -> generated C++
#   -> harness from fixture annotations -> clang host build -> run -> report
#   -> same corpus cross-compiled for PS5 (execution: see tools/ps5_run_cpu.sh).
# Re-runnable from scratch; never edits generated C++.
#
# Env: RCOMP_DEPS (default ../deps), PS5_PAYLOAD_SDK (default $RCOMP_DEPS/sdk/ps5-payload-sdk),
#      RCOMP_SKIP_PS5=1 to skip the cross build.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
deps="${RCOMP_DEPS:-$(dirname "$root")/deps}"
sdk="${PS5_PAYLOAD_SDK:-$deps/sdk/ps5-payload-sdk}"
b="$root/build"
mkdir -p "$b/reports"
step() { echo "==> $*" >&2; }

step "M0: build XenonRecomp (host)"
git -C "$root" submodule update --init --recursive third_party/XenonRecomp
"$root/cpu/tools/prepare_xenonrecomp.sh" | tee "$b/reports/xenonrecomp-patches.txt"
cmake -S "$b/cpu-xenonrecomp-src" -B "$b/cpu-xenonrecomp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >"$b/cpu-xenonrecomp.cfg.log"
ninja -C "$b/cpu-xenonrecomp" XenonRecomp XenonAnalyse >"$b/cpu-xenonrecomp.build.log"

step "M1: VMX128 host assembler (binutils 2.24 + Xenia patch)"
"$root/cpu/tools/build_binutils_vmx128.sh"
step "M1: assemble fixtures + run XenonRecomp"
command -v powerpc-linux-gnu-as >/dev/null || { echo "BLOCKED: powerpc-linux-gnu binutils missing (apt install binutils-powerpc-linux-gnu)" >&2; exit 2; }
xenia_tests="$deps/xenia/src/xenia/cpu/ppc/testing"
[[ -d $xenia_tests ]] || { echo "BLOCKED: $xenia_tests missing; run tools/fetch_deps.sh" >&2; exit 2; }
python3 "$root/cpu/tools/build_corpus.py" --set "xenia=$xenia_tests" --set "rcomp=$root/fixtures/ppc/rcomp" \
        --vmx128-as "$b/cpu-binutils-vmx128/bin/powerpc-none-elf-as" \
        --out "$b/cpu-corpus" --xenonrecomp "$b/cpu-xenonrecomp/XenonRecomp/XenonRecomp" \
        --xenonrecomp-src "$root/third_party/XenonRecomp" --patches "$root/cpu/patches/xenonrecomp"

step "M1: generate harness from fixture annotations"
python3 "$root/tests/cpu/gen_harness.py" --corpus "$b/cpu-corpus" --out "$b/tests-cpu-cases"

cfg=(-DRCOMP_ROOT="$root" -DCORPUS_DIR="$b/cpu-corpus" -DCASES_DIR="$b/tests-cpu-cases")
step "M1: float16 helpers vs host F16C (exhaustive)"
clang++ -std=c++17 -O2 -march=x86-64-v3 -I "$b/cpu-xenonrecomp-src/XenonUtils" \
    -I "$b/cpu-xenonrecomp-src/thirdparty/simde" "$root/cpu/tests/test_f16_exhaustive.cpp" -o "$b/cpu-test-f16"
"$b/cpu-test-f16" | tee "$b/reports/f16-exhaustive.txt"
step "M1: guest timebase (PPC_MFTB) runs at 50 MHz"
clang++ -std=c++17 -O2 -I "$root/include" "$root/cpu/tests/test_timebase.cpp" -o "$b/cpu-test-timebase"
"$b/cpu-test-timebase" | tee "$b/reports/timebase.txt"

step "M1: host build + run"
cmake -S "$root/tests/cpu" -B "$b/tests-cpu-host" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "${cfg[@]}" >"$b/tests-cpu-host.cfg.log"
ninja -C "$b/tests-cpu-host" >"$b/tests-cpu-host.build.log"
python3 "$root/tests/cpu/run_cpu_tests.py" --binary "$b/tests-cpu-host/rcomp_cpu_tests" --corpus "$b/cpu-corpus" \
        --cases "$b/tests-cpu-cases" --root "$root" --report "$b/reports/cpu-host.json" --label host \
        --isa "-march=x86-64-v3 -ffp-contract=on"

step "M3: guest program + HLE (host)"
python3 "$root/cpu/tools/build_corpus.py" --set "m3=$root/fixtures/ppc/m3" --out "$b/m3-corpus" \
        --xenonrecomp "$b/cpu-xenonrecomp/XenonRecomp/XenonRecomp" --xenonrecomp-src "$root/third_party/XenonRecomp"
python3 "$root/tests/cpu/gen_harness.py" --corpus "$b/m3-corpus" --out "$b/m3-cases"
cmake -S "$root/tests/m3" -B "$b/tests-m3-host" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DRCOMP_ROOT="$root" -DCORPUS_DIR="$b/m3-corpus" -DCASES_DIR="$b/m3-cases" >"$b/tests-m3-host.cfg.log"
ninja -C "$b/tests-m3-host" >"$b/tests-m3-host.build.log"
"$b/tests-m3-host/rcomp_m3_tests" | tee "$b/reports/m3-host.jsonl"

step "Sync: lwarx/stwcx. under contention (host)"
python3 "$root/cpu/tools/build_corpus.py" --set "sync=$root/fixtures/ppc/sync" --out "$b/sync-corpus" \
        --xenonrecomp "$b/cpu-xenonrecomp/XenonRecomp/XenonRecomp" --xenonrecomp-src "$root/third_party/XenonRecomp"
python3 "$root/tests/cpu/gen_harness.py" --corpus "$b/sync-corpus" --out "$b/sync-cases"
cmake -S "$root/tests/sync" -B "$b/tests-sync-host" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DRCOMP_ROOT="$root" -DCORPUS_DIR="$b/sync-corpus" -DCASES_DIR="$b/sync-cases" >"$b/tests-sync-host.cfg.log"
ninja -C "$b/tests-sync-host" >"$b/tests-sync-host.build.log"
"$b/tests-sync-host/rcomp_sync_tests" | tee "$b/reports/sync-host.jsonl"

step "M6 rehearsal: synthetic XEX -> XenonRecomp XEX mode -> runtime loader (host)"
# XenonRecomp (upstream) segfaults when out_directory_path does not exist yet.
rm -rf "$b/xex" && mkdir -p "$b/xex/ppc"
python3 "$root/cpu/tools/mkxex.py" "$root/fixtures/xex/rcomp_title.s" --out "$b/xex/rcomp_title"
cp "$root/fixtures/xex/rcomp_title.toml" "$b/xex/"
# Jump tables from XenonAnalyse and the register save/restore helper addresses
# (from the linker's symbols), as a real title's TOML carries them.
"$b/cpu-xenonrecomp/XenonAnalyse/XenonAnalyse" "$b/xex/rcomp_title.xex" "$b/xex/switch_tables.toml" >"$b/xex/xenonanalyse.log"
python3 - "$b/xex" <<'PY'
import json, sys
d = sys.argv[1]
f = json.load(open(f"{d}/rcomp_title.json"))["functions"]
with open(f"{d}/rcomp_title.toml", "a") as t:
    t.write('switch_table_file_path = "switch_tables.toml"\n')
    t.write(f'savegprlr_14_address = 0x{f["__savegprlr_14"]:08X}\n')
    t.write(f'restgprlr_14_address = 0x{f["__restgprlr_14"]:08X}\n')
PY
[[ $(grep -c '^\[\[switch\]\]' "$b/xex/switch_tables.toml") == 4 ]] || { echo "FAIL: XenonAnalyse did not find the 4 jump tables" >&2; exit 1; }
(cd "$b/xex" && "$b/cpu-xenonrecomp/XenonRecomp/XenonRecomp" rcomp_title.toml \
     "$b/cpu-xenonrecomp-src/XenonUtils/ppc_context.h" >"$b/xex/xenonrecomp.log" 2>&1)
! grep -qi "unimplemented" "$b/xex/xenonrecomp.log" || { echo "FAIL: XenonRecomp reported unimplemented instructions" >&2; exit 1; }
cmake -S "$root/tests/xex" -B "$b/tests-xex-host" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DRCOMP_ROOT="$root" -DXEX_GEN_DIR="$b/xex/ppc" -DRCOMP_XEX="$b/xex/rcomp_title.xex" >"$b/tests-xex-host.cfg.log"
ninja -C "$b/tests-xex-host" >"$b/tests-xex-host.build.log"
"$b/tests-xex-host/rcomp_xex_tests" | tee "$b/reports/xex-host.jsonl"
# Graphics variant (fixtures/xex/rcomp_gfx_title.s): recompiled here; it runs
# through the game shell with the GPU in tests/xex_gfx (tools/check_all.sh,
# check xex-gfx) and in the PS5 kit (title 11).
rm -rf "$b/xex-gfx" && mkdir -p "$b/xex-gfx/ppc"
python3 "$root/cpu/tools/mkxex.py" "$root/fixtures/xex/rcomp_gfx_title.s" --out "$b/xex-gfx/rcomp_gfx_title"
cp "$root/fixtures/xex/rcomp_gfx_title.toml" "$b/xex-gfx/"
mkdir -p "$b/xex-gfx/ppc"
(cd "$b/xex-gfx" && "$b/cpu-xenonrecomp/XenonRecomp/XenonRecomp" rcomp_gfx_title.toml \
     "$b/cpu-xenonrecomp-src/XenonUtils/ppc_context.h" >"$b/xex-gfx/xenonrecomp.log" 2>&1)
! grep -qi "unimplemented" "$b/xex-gfx/xenonrecomp.log" || { echo "FAIL: XenonRecomp reported unimplemented instructions (gfx XEX)" >&2; exit 1; }
[[ -f $b/xex-gfx/ppc/ppc_func_mapping.cpp ]] || { echo "FAIL: no generated code for the gfx XEX" >&2; exit 1; }

step "M6 rehearsal: encrypted/compressed XEX -> rcomp_xex_decode (XenonUtils, patch 0006) -> same runner"
xinc=(-I "$b/cpu-xenonrecomp-src/XenonUtils" -I "$b/cpu-xenonrecomp-src/thirdparty/simde"
      -I "$b/cpu-xenonrecomp-src/thirdparty/tiny-AES-c" -I "$b/cpu-xenonrecomp-src/thirdparty/fmt/include"
      -I "$b/cpu-xenonrecomp-src/thirdparty/TinySHA1")
xlibs=("$b/cpu-xenonrecomp/XenonUtils/libXenonUtils.a" "$b/cpu-xenonrecomp/thirdparty/disasm/libdisasm.a"
       "$b/cpu-xenonrecomp/thirdparty/fmt/libfmt.a")
clang++ -std=c++20 -O2 -Wall -Werror "${xinc[@]}" "$root/cpu/tools/xex_decode.cpp" "${xlibs[@]}" -o "$b/cpu-xex-decode"
clang++ -std=c++20 -O2 -Wall -Werror "${xinc[@]}" -DRCOMP_REPO_ROOT="\"$root\"" "$root/cpu/tests/test_xex_decode.cpp" \
    "${xlibs[@]}" -o "$b/cpu-test-xex-decode"
"$b/cpu-test-xex-decode" "$b/xex/rcomp_title.xex" "$b/xex/rcomp_title.enc.xex" | tee "$b/reports/xex-decode.txt"
"$b/cpu-xex-decode" "$b/xex/rcomp_title.enc.xex" "$b/xex/rcomp_title.decoded.xex"
"$b/tests-xex-host/rcomp_xex_tests" "$b/xex/rcomp_title.decoded.xex" | tee "$b/reports/xex-decoded-host.jsonl"
# XenonRecomp reads the encrypted/compressed variant itself: same generated code.
mkdir -p "$b/xex/enc/ppc" && cp "$b/xex/rcomp_title.enc.xex" "$b/xex/enc/rcomp_title.xex" && cp "$b/xex/rcomp_title.toml" "$b/xex/switch_tables.toml" "$b/xex/enc/"
(cd "$b/xex/enc" && "$b/cpu-xenonrecomp/XenonRecomp/XenonRecomp" rcomp_title.toml \
     "$b/cpu-xenonrecomp-src/XenonUtils/ppc_context.h" >"$b/xex/enc/xenonrecomp.log" 2>&1)
diff -r "$b/xex/ppc" "$b/xex/enc/ppc" >/dev/null || { echo "FAIL: generated code differs for the encrypted XEX" >&2; exit 1; }

step "M6 inventory on the encrypted + LZX synthetic XEX (tools/m6_inventory.py)"
rm -rf "$b/m6-selftest"
python3 "$root/tools/m6_inventory.py" "$b/xex/rcomp_title.enc.xex" --out "$b/m6-selftest" --recompile >/dev/null
python3 - "$b/m6-selftest/inventory.json" "$b/xex/rcomp_title.json" <<'PY' | tee "$b/reports/m6-inventory-selftest.txt"
import json, sys
inv, sym = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))["functions"]
k = inv["imports"]["xboxkrnl.exe"]
checks = {
    "jump_tables == 4": inv["jump_tables"]["tables"] == 4,
    "savegprlr_14 unique": inv["register_helpers"]["savegprlr_14_address"]["unique"],
    "restgprlr_14 unique": inv["register_helpers"]["restgprlr_14_address"]["unique"],
    "no fpr/vmx helper false positive": all(v["matches"] == 0 for n, v in inv["register_helpers"].items() if "gpr" not in n),
    "imports implemented": k["functions_implemented"] == ["NtAllocateVirtualMemory", "NtFreeVirtualMemory"],
    "variables": k["variables"] == ["XboxHardwareInfo", "XboxKrnlVersion"],
    "xbdm unknown ordinal": inv["imports"]["xbdm.xex"]["unknown_ordinals"] == ["0x0012"],
    "recompiled cleanly": inv["xenonrecomp"]["exit_code"] == 0 and not inv["xenonrecomp"]["unrecognized_instructions"],
}
toml = open(sys.argv[1].replace("inventory.json", "game.toml")).read()
checks["helper addresses match the linker"] = (f"savegprlr_14_address = 0x{sym['__savegprlr_14']:08X}" in toml and
                                               f"restgprlr_14_address = 0x{sym['__restgprlr_14']:08X}" in toml)
for n, ok in checks.items():
    print(f"m6_inventory/{n:40s} {'PASS' if ok else 'FAIL'}")
sys.exit(0 if all(checks.values()) else 1)
PY

if [[ "${RCOMP_SKIP_PS5:-0}" != 1 ]]; then
    step "M2: PS5 cross build (execution requires a console: NOT TESTED here)"
    [[ -x $sdk/bin/prospero-clang++ ]] || { echo "BLOCKED: no PS5 payload SDK at $sdk" >&2; exit 2; }
    cmake -S "$root/tests/cpu" -B "$b/tests-cpu-ps5" -G Ninja \
          -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/ps5.cmake" "${cfg[@]}" >"$b/tests-cpu-ps5.cfg.log"
    ninja -C "$b/tests-cpu-ps5" >"$b/tests-cpu-ps5.build.log"
    sha256sum "$b/tests-cpu-ps5/rcomp_cpu_tests" | tee "$b/reports/cpu-ps5-binary.sha256"
fi
