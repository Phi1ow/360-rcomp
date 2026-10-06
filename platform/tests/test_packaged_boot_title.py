"""Run the real title builder's prerequisite checks with original boot inputs.

Run under the host bash/Python environment with --sdk, --xex-build, --xex and
--data from a tests/xex RCOMP_XEX_PACKAGED_BOOT=ON build. No compiler or console
command should be reached by any negative case.
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
    for option in ("sdk", "xex-build", "xex", "data"):
        ap.add_argument("--" + option, type=pathlib.Path, required=True)
    args = ap.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    builder = root / "platform/ps5/tools/build_title.sh"
    artifact = root / "build/platform-ps5-title-boot/dist/PPSA88360/eboot.bin"
    old_artifact = hashlib.sha256(artifact.read_bytes()).hexdigest() if artifact.exists() else None
    env = dict(os.environ, RCOMP_PS5_SDK=str(args.sdk.resolve()),
               RCOMP_XEX_BUILD=str(args.xex_build.resolve()),
               RCOMP_XEX_FILE=str(args.xex.resolve()),
               RCOMP_XEX_BOOT_DATA_FILE=str(args.data.resolve()),
               RCOMP_XEX_PACKAGED_BOOT="1", RCOMP_XEX_POOL_RTL="0",
               RCOMP_XEX_MODULES="0", RCOMP_XEX_CALENDAR_THREADS="0",
               RCOMP_XEX_BOOT_REHEARSAL="0", RCOMP_TITLE_CPU_CORPUS="0",
               RCOMP_TITLE_M3="0", RCOMP_TITLE_XENOS_CP="0", RCOMP_TITLE_XEX="0")
    scratch = root / "build/platform-ps5-preflight-tests"
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
        actual = hashlib.sha256(artifact.read_bytes()).hexdigest() if artifact.exists() else None
        assert actual == old_artifact, (name, "existing boot title changed")
        print(f"PASS {name}: exit=2 before build; existing title unchanged")
        passed += 1

    with tempfile.TemporaryDirectory(dir=scratch) as temporary:
        temp = pathlib.Path(temporary)
        wrong = temp / "wrong.bin"
        wrong.write_bytes(bytes((1, 2, 3, 5)))
        reject("missing-data-setting", {"RCOMP_XEX_BOOT_DATA_FILE": ""}, "must name")
        reject("missing-data-file", {"RCOMP_XEX_BOOT_DATA_FILE": str(temp / "missing.bin")}, "must name")
        reject("wrong-data", {"RCOMP_XEX_BOOT_DATA_FILE": str(wrong)}, "exactly 01 02 03 04")
        reject("missing-xex-mode", {}, "requires --xex", switches=())
        reject("invalid-boot-mode", {"RCOMP_XEX_PACKAGED_BOOT": "2"}, "must be 0 or 1")
        partial = temp / "partial-build"
        names = ("librcomp_xex_selftest.a", "librcomp_xex_generated.a",
                 "runtime/librcomp_runtime.a", "runtime/librcomp_runtime_video.a",
                 "platform/librcomp_platform_input.a")
        for name in names:
            dest = partial / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.xex_build / name, dest)
        for name in ("runtime/librcomp_runtime_video.a", "platform/librcomp_platform_input.a"):
            (partial / name).unlink()
            reject("missing-" + pathlib.Path(name).name, {"RCOMP_XEX_BUILD": str(partial)},
                   "missing " + str(partial / name))
            shutil.copyfile(args.xex_build / name, partial / name)
    print(f"PASS packaged boot title prerequisites {passed}/{passed}")


if __name__ == "__main__":
    main()
