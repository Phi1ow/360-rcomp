"""Exercise the real pool/Rtl title builder's prerequisite rejection paths.

Supply real --sdk, --xex-build and --xex paths. No fake compiler or success
substitute is used. Every case must stop before compilation or title changes.
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for option in ("sdk", "xex-build", "xex"):
        ap.add_argument("--" + option, type=pathlib.Path, required=True)
    args = ap.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    builder = root / "platform/ps5/tools/build_title.sh"
    artifacts = [root / f"build/platform-ps5-title-{mode}/dist/PPSA88360/eboot.bin"
                 for mode in ("boot", "pool-rtl")]

    def hashes():
        return [hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
                for p in artifacts]

    previous = hashes()
    env = dict(os.environ, RCOMP_PS5_SDK=str(args.sdk.resolve()),
               RCOMP_XEX_BUILD=str(args.xex_build.resolve()),
               RCOMP_XEX_FILE=str(args.xex.resolve()), RCOMP_XEX_BOOT_DATA_FILE="",
               RCOMP_XEX_PACKAGED_BOOT="0", RCOMP_XEX_POOL_RTL="1",
               RCOMP_XEX_MODULES="0", RCOMP_XEX_CALENDAR_THREADS="0", RCOMP_XEX_BOOT_REHEARSAL="0",
               RCOMP_TITLE_CPU_CORPUS="0", RCOMP_TITLE_M3="0",
               RCOMP_TITLE_XENOS_CP="0", RCOMP_TITLE_XEX="0")
    scratch = root / "build/platform-pool-rtl-preflight-tests"
    scratch.mkdir(exist_ok=True)
    passed = 0

    def reject(name, update, expected, switches=("--xex",)):
        nonlocal passed
        run = subprocess.run(["bash", str(builder), *switches], cwd=root,
                             env=dict(env, **update), text=True, capture_output=True, timeout=10)
        output = run.stdout + run.stderr
        assert run.returncode == 2, (name, run.returncode, output)
        assert expected in output, (name, output)
        assert "step 1/6" not in output, (name, output)
        assert hashes() == previous, (name, "existing title changed")
        print(f"PASS {name}: exit=2 before build; existing titles unchanged")
        passed += 1

    with tempfile.TemporaryDirectory(dir=scratch) as temporary:
        temp = pathlib.Path(temporary)
        reject("invalid-pool-mode", {"RCOMP_XEX_POOL_RTL": "2"}, "must be 0 or 1")
        for option in ("CPU_CORPUS", "M3", "XEX", "XENOS_CP"):
            key = "RCOMP_TITLE_" + option
            reject("invalid-" + key, {key: "2"}, key + " must be 0 or 1", switches=())
        reject("unknown-switch", {}, "usage:", switches=("--unknown",))
        reject("missing-xex-mode", {}, "requires --xex", switches=())
        reject("conflicting-boot-mode", {"RCOMP_XEX_PACKAGED_BOOT": "1"}, "mutually exclusive")
        reject("missing-xex-file", {"RCOMP_XEX_FILE": str(temp / "missing.xex")}, "missing " + str(temp / "missing.xex"))
        partial = temp / "partial-build"
        names = ("librcomp_xex_selftest.a", "librcomp_xex_generated.a",
                 "runtime/librcomp_runtime.a", "runtime/librcomp_runtime_video.a",
                 "platform/librcomp_platform_input.a")
        for name in names:
            dest = partial / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.xex_build / name, dest)
        for name in names:
            (partial / name).unlink()
            reject("missing-" + pathlib.Path(name).name, {"RCOMP_XEX_BUILD": str(partial)},
                   "missing " + str(partial / name))
            shutil.copyfile(args.xex_build / name, partial / name)
    print(f"PASS pool/Rtl title prerequisites {passed}/{passed}")


if __name__ == "__main__":
    main()
