#!/usr/bin/env bash
# Every host check of the project in one run, with one verdict per check
# (PASS / FAIL / BLOCKED). Host evidence only: nothing here is a PS5 proof.
#
#   tools/check_all.sh            (RCOMP_SKIP_PS5=1 skips the PS5 cross build
#                                  inside cpu_pipeline.sh; RCOMP_SKIP_XENOS=1
#                                  skips the Xenos shader checks, which fetch
#                                  XenosRecomp submodules)
#
# Checks:
#   cpu       tools/cpu_pipeline.sh: XenonRecomp + corpus (M1), f16 exhaustive,
#             M3 program, lwarx/stwcx. contention, synthetic XEX (M6 rehearsal),
#             PS5 cross build of the corpus (M2 compile)
#   platform  guest memory, POSIX backend + PS5 backend on a test double (ctest)
#   runtime   runtime unit tests (ctest)
#   fps-overlay tests/fps_overlay: text of the on-screen frame-rate counter (ctest, host only)
#   tools     tools/tests: disc catalog (header/listing parsing, aggregation,
#             ranking, sanitized report), host diagnostics (Python, host only)
#   perf-tools tests/perf_tools: frame-rate counter parsing, A/B statistics, scene alignment, PC-profile comparison, the register-input scanner and the Xenos pass-accounting analysis (Python, host only)
#   m5        M5 slice on the host Vulkan ICD (lavapipe); BLOCKED without one
#   xenos     gpu/xenos/tools/run_xenos_checks.sh
#   xenos-gpu gpu/xenos/rexglue/check.sh (Xenia/rexglue shader translator)
#   xenos-cp  tests/xenos_cp: rexglue PM4 command processor over guest memory,
#             then the kernel Vd* exports + physical memory driving it
#             (recording TESTDOUBLE backend, no Vulkan)
#   xenos-vk  tests/xenos_cp/test_vulkan.cpp: the real Vulkan backend on the
#             host Vulkan ICD (lavapipe); BLOCKED without one
#   xex-gfx   tests/xex_gfx: recompiled synthetic graphics XEX through the
#             game shell (app/): kernel -> GPU -> Vulkan -> screen (headless)
# Report: build/reports/check_all.txt. Exit 0 iff no FAIL.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
b="$root/build"
mkdir -p "$b/reports"
report="$b/reports/check_all.txt"
: >"$report"
fails=0

verdict() {  # name status detail
    printf '%-9s %-8s %s\n' "$1" "$2" "$3" | tee -a "$report"
    [[ $2 == FAIL ]] && fails=$((fails + 1))
    return 0
}
run() {  # name log cmd...
    local name=$1 log=$2
    shift 2
    echo "==> [check] $name (log: $log)" >&2
    "$@" >"$log" 2>&1
}
ctest_dir() {  # name src build
    run "$1" "$b/check-$1.log" bash -c "cmake -S '$2' -B '$3' -G Ninja -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ -DRCOMP_ROOT='$root' && ninja -C '$3' && ctest --test-dir '$3' --output-on-failure"
    local rc=$?
    verdict "$1" "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" \
        "$(grep -Eo '[0-9]+% tests passed.*' "$b/check-$1.log" | tail -1)"
}

# --- cpu ---------------------------------------------------------------------
run cpu "$b/check-cpu.log" "$root/tools/cpu_pipeline.sh"
rc=$?
case $rc in
    0) st=PASS ;;
    2) st=BLOCKED ;;
    *) st=FAIL ;;
esac
detail=$(python3 - "$b/reports" <<'PY'
import collections, json, os, sys
d = sys.argv[1]
parts = []
try:
    r = json.load(open(os.path.join(d, "cpu-host.json")))
    c = collections.Counter(x["status"] for x in r["results"])
    parts.append("corpus " + "/".join(f"{c[k]} {k}" for k in ("PASS", "FAIL", "BLOCKED")))
    bad = c["FAIL"] > 0
except Exception as e:
    parts.append(f"corpus report unreadable ({e})")
    bad = True
for name in ("m3-host.jsonl", "sync-host.jsonl", "xex-host.jsonl", "xex-decoded-host.jsonl"):
    try:
        rows = [json.loads(l) for l in open(os.path.join(d, name)) if l.startswith("{")]
        ok = sum(x.get("status") == "PASS" for x in rows)
        parts.append(f"{name.rsplit('-host', 1)[0]} {ok}/{len(rows)} PASS")
        bad = bad or not rows or ok != len(rows)
    except Exception:
        parts.append(f"{name} missing")
try:
    lines = open(os.path.join(d, "xex-decode.txt")).read().split("\n")
    parts.append("xex-decode " + "/".join(f"{sum(k in l for l in lines)} {k}" for k in ("PASS", "FAIL", "NOT TESTED")))
    bad = bad or any(" FAIL" in l for l in lines)
except Exception:
    parts.append("xex-decode.txt missing")
if bad:
    parts.append("FAILED")
print("; ".join(parts))
PY
)
# A missing sub-report is a failure even if the script exited 0.
[[ $st == PASS && ( $detail == *missing* || $detail == *FAILED* ) ]] && st=FAIL
verdict cpu "$st" "$detail"

# --- platform / runtime ---------------------------------------------------------
ctest_dir platform "$root/platform" "$b/check-platform-host"
ctest_dir runtime "$root/runtime" "$b/check-runtime-host"
ctest_dir fps-overlay "$root/tests/fps_overlay" "$b/check-fps-overlay-host"
ctest_dir launch-menu "$root/tests/launch_menu" "$b/check-launch-menu-host"
run tools "$b/check-tools.log" python3 -m unittest discover -s "$root/tools/tests" -p "test_*.py"
rc=$?
verdict tools "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" "disc catalog extraction parsing, aggregation and sanitized report; host diagnostics (HOST ONLY)"
run perf-tools "$b/check-perf-tools.log" python3 -m unittest discover -s "$root/tests/perf_tools" -p "test_*.py"
rc=$?
verdict perf-tools "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" "counter parsing, A/B statistics, scene alignment, PC-profile comparison, register-input scanner, Xenos pass-accounting analysis (HOST ONLY)"

# --- m5 (host Vulkan) -----------------------------------------------------------
if [[ ! -d $b/m5-corpus ]]; then
    python3 "$root/cpu/tools/build_corpus.py" --set "m5=$root/fixtures/ppc/m5" --out "$b/m5-corpus" \
        --xenonrecomp "$b/cpu-xenonrecomp/XenonRecomp/XenonRecomp" \
        --xenonrecomp-src "$root/third_party/XenonRecomp" --patches "$root/cpu/patches/xenonrecomp" \
        >"$b/check-m5-corpus.log" 2>&1
    python3 "$root/tests/cpu/gen_harness.py" --corpus "$b/m5-corpus" --out "$b/m5-cases" \
        >>"$b/check-m5-corpus.log" 2>&1
fi
if ! ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1; then
    verdict m5 BLOCKED "no host Vulkan ICD (install mesa-vulkan-drivers for lavapipe)"
else
    run m5 "$b/check-m5.log" bash -c "cmake -S '$root/tests/m5' -B '$b/tests-m5-host' -G Ninja \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DRCOMP_ROOT='$root' \
        -DCORPUS_DIR='$b/m5-corpus' -DCASES_DIR='$b/m5-cases' && ninja -C '$b/tests-m5-host' && \
        '$b/tests-m5-host/rcomp_m5_tests'"
    rc=$?
    verdict m5 "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" \
        "$(grep -Eo 'RCOMP-M5 image.*' "$b/check-m5.log" | tail -1) (HOST ONLY)"
fi

# --- xenos ---------------------------------------------------------------------
if [[ ${RCOMP_SKIP_XENOS:-0} == 1 ]]; then
    verdict xenos "NOT TESTED" "RCOMP_SKIP_XENOS=1"
else
    run xenos "$b/check-xenos.log" "$root/gpu/xenos/tools/run_xenos_checks.sh"
    rc=$?
    verdict xenos "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" "synthetic containers only (no game shaders)"
fi

# --- xenos GPU (rexglue translator) ------------------------------------------
if ! command -v spirv-val >/dev/null; then
    verdict xenos-gpu BLOCKED "spirv-val missing (apt install spirv-tools)"
else
    run xenos-gpu "$b/check-xenos-gpu.log" "$root/gpu/xenos/rexglue/check.sh"
    rc=$?
    verdict xenos-gpu "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" \
        "$(grep -c ' PASS$' "$b/check-xenos-gpu.log") synthetic shaders -> SPIR-V valid for Vulkan 1.0 (standard layout)"
fi

# --- xenos CP (rexglue command processor) -------------------------------------
if [[ ${RCOMP_SKIP_XENOS:-0} == 1 ]]; then
    verdict xenos-cp "NOT TESTED" "RCOMP_SKIP_XENOS=1"
else
    run xenos-cp "$b/check-xenos-cp.log" bash -c "'$root/gpu/xenos/rexglue/prepare.sh' && \
        cmake -S '$root/tests/xenos_cp' -B '$b/tests-xenos-cp' -G Ninja -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ -DRCOMP_ROOT='$root' && ninja -C '$b/tests-xenos-cp' rcomp_xenos_cp_tests rcomp_xenos_video_tests && \
        '$b/tests-xenos-cp/rcomp_xenos_cp_tests' && '$b/tests-xenos-cp/rcomp_xenos_video_tests'"
    rc=$?
    verdict xenos-cp "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" \
        "$(grep -Eo 'RCOMP-XENOS-(CP|VIDEO).*' "$b/check-xenos-cp.log" | tr '\n' ' ')(Vd* + PM4 -> recording backend, HOST ONLY)"
fi

# --- xenos Vulkan backend (host ICD) ------------------------------------------
if [[ ${RCOMP_SKIP_XENOS:-0} == 1 ]]; then
    verdict xenos-vk "NOT TESTED" "RCOMP_SKIP_XENOS=1"
elif ! ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1; then
    verdict xenos-vk BLOCKED "no host Vulkan ICD (install mesa-vulkan-drivers for lavapipe)"
else
    run xenos-vk "$b/check-xenos-vk.log" bash -c "ninja -C '$b/tests-xenos-cp' rcomp_xenos_vulkan_tests && \
        '$b/tests-xenos-cp/rcomp_xenos_vulkan_tests'"
    rc=$?
    case $rc in 0) st=PASS ;; 3) st=BLOCKED ;; *) st=FAIL ;; esac
    verdict xenos-vk "$st" "$(grep -Eo 'RCOMP-XENOS-VK.*' "$b/check-xenos-vk.log" | tail -1)"
fi

# --- game shell: recompiled graphics XEX -> screen ----------------------------
if [[ ${RCOMP_SKIP_XENOS:-0} == 1 ]]; then
    verdict xex-gfx "NOT TESTED" "RCOMP_SKIP_XENOS=1"
elif ! ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1; then
    verdict xex-gfx BLOCKED "no host Vulkan ICD"
else
    run xex-gfx "$b/check-xex-gfx.log" bash -c "cmake -S '$root/tests/xex_gfx' -B '$b/tests-xex-gfx-host' -G Ninja \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DRCOMP_ROOT='$root' -DXEX_GEN_DIR='$b/xex-gfx/ppc' \
        -DRCOMP_XEX='$b/xex-gfx/rcomp_gfx_title.xex' && ninja -C '$b/tests-xex-gfx-host' rcomp_xex_gfx_tests && \
        '$b/tests-xex-gfx-host/rcomp_xex_gfx_tests'"
    rc=$?
    verdict xex-gfx "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" \
        "$(grep -Eo 'RCOMP-XEX-GFX.*' "$b/check-xex-gfx.log" | tail -1) (recompiled PPC -> kernel -> GPU -> Vulkan -> screen, HOST ONLY)"
fi

echo "PS5 execution: NOT TESTED by this script" | tee -a "$report"
exit $(( fails ? 1 : 0 ))
