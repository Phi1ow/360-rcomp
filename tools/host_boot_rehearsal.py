#!/usr/bin/env python3
"""Original PPC -> actual M6 tools -> generated C++ -> production TitleRuntime.

Run with a POSIX toolchain (including repository-local Cygwin). Requires the
pinned patched XenonRecomp, decoder and powerpc-linux-gnu binutils on PATH.
Always uses a fresh output directory; never deploys or accesses a console.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from m6_inventory import output_allowed

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--xenon-source", required=True, type=Path)
    parser.add_argument("--xenon-build", required=True, type=Path)
    parser.add_argument("--decode-tool", required=True, type=Path)
    parser.add_argument("--mode", choices=("boot", "pool-rtl", "modules", "calendar-threads", "services", "kernel-boot"), default="boot",
                        help="original PPC program and its dedicated production-runtime oracle")
    parser.add_argument("--ubsan", action="store_true", help="undefined behavior sanitizer, trap mode")
    args = parser.parse_args()
    out = args.out.absolute()
    if not output_allowed(out) or not out.resolve().is_relative_to((ROOT / "build").resolve()) or out.exists():
        parser.error("output must be a new safe directory under this repository build/")
    out.mkdir(parents=True)
    fixture, inventory, compiled = out / "fixture", out / "inventory", out / "host"
    fixture.mkdir()
    source, generator, decoder = args.xenon_source.resolve(), args.xenon_build.resolve(), args.decode_tool.resolve()
    program, cmake_mode = {
        "boot": ("rcomp_boot", "RCOMP_XEX_BOOT_REHEARSAL"),
        "pool-rtl": ("rcomp_pool_rtl", "RCOMP_XEX_POOL_RTL"),
        "modules": ("rcomp_modules", "RCOMP_XEX_MODULES"),
        "calendar-threads": ("rcomp_calendar_threads", "RCOMP_XEX_CALENDAR_THREADS"),
        "services": ("rcomp_services", "RCOMP_XEX_SERVICES"),
        "kernel-boot": ("rcomp_kernel_boot", "RCOMP_XEX_KERNEL_BOOT"),
    }[args.mode]
    steps = [
        ("original_xex", [sys.executable, str(ROOT / "cpu/tools/mkxex.py"), str(ROOT / "fixtures/xex" / (program + ".s")), "--out", str(fixture / program)]),
        ("m6_inventory", [sys.executable, str(ROOT / "tools/m6_inventory.py"), str(fixture / (program + ".xex")), "--out", str(inventory), "--require-supported",
                          "--decode-tool", str(decoder), "--analyse-tool", str(generator / "XenonAnalyse/XenonAnalyse"),
                          "--recomp-tool", str(generator / "XenonRecomp/XenonRecomp"), "--xenon-source", str(source)]),
        ("m6_gate", [sys.executable, str(ROOT / "tools/m6_boot_gate.py"), str(inventory)]),
        ("configure", ["cmake", "-S", str(ROOT / "tests/xex"), "-B", str(compiled), "-G", "Ninja",
                       "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-D" + cmake_mode + "=ON",
                       "-DRCOMP_ROOT=" + str(ROOT), "-DRCOMP_XENONRECOMP_SRC=" + str(source),
                       "-DXEX_GEN_DIR=" + str(inventory / "ppc"), "-DRCOMP_XEX=" + str(inventory / "plain.xex")]
                       + (["-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fsanitize-trap=undefined"] if args.ubsan else [])),
        ("build", ["cmake", "--build", str(compiled), "-j", "8"]),
        ("test", ["ctest", "--test-dir", str(compiled), "--no-tests=error", "--output-on-failure", "-V"]),
    ]
    if args.mode == "services":
        steps.insert(3, ("original_io_data", [sys.executable,
            str(ROOT / "tests/xex/prepare_services_data.py"),
            "--out", str(inventory / "boot-data.bin")]))
    report = {"scope": "original PPC host runtime rehearsal", "program": program, "status": "FAIL", "steps": [],
              "ubsan": args.ubsan, "vulkan_rendering": "NOT TESTED", "ps5_execution": "NOT TESTED", "gta_iv": "NOT TESTED"}
    for name, command in steps:
        with (out / (name + ".log")).open("w") as log:
            try:
                result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=300)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
            except OSError as error:
                log.write(str(error)); code = 127
        report["steps"].append({"name": name, "command": command, "exit_code": code})
        (out / "RESULTS.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"{'PASS' if code == 0 else 'FAIL'} host/{name}: exit={code}, log={out / (name + '.log')}", flush=True)
        if code: return 1
    report["status"] = "PASS"
    (out / "RESULTS.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
