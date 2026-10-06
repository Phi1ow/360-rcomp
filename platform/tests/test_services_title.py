"""Exercise services-title prerequisite failures against the real builder."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "xex-build", "xex"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    builder = root / "platform/ps5/tools/build_title.sh"
    titles = list(root.glob("build/platform-ps5-title*/dist/PPSA88360"))
    new_title = root / "build/platform-ps5-title-services/dist/PPSA88360"
    if new_title not in titles:
        titles.append(new_title)

    def snapshot():
        return {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                for title in titles for path in title.rglob("*") if path.is_file()}

    original = snapshot()
    environment = dict(os.environ, RCOMP_PS5_SDK=str(args.sdk.resolve()),
                       RCOMP_XEX_BUILD=str(args.xex_build.resolve()),
                       RCOMP_XEX_FILE=str(args.xex.resolve()), RCOMP_XEX_SERVICES="1")
    for suffix in ("PACKAGED_BOOT", "POOL_RTL", "MODULES", "CALENDAR_THREADS", "BOOT_REHEARSAL"):
        environment["RCOMP_XEX_" + suffix] = "0"
    for suffix in ("CPU_CORPUS", "M3", "XENOS_CP", "XEX"):
        environment["RCOMP_TITLE_" + suffix] = "0"
    scratch = root / "build/platform-services-prerequisite-tests"
    scratch.mkdir(exist_ok=True)
    passed = 0

    def reject(name, settings, diagnostic, switches=("--xex",)):
        nonlocal passed
        result = subprocess.run(["bash", str(builder), *switches], cwd=root,
                                env=dict(environment, **settings), text=True,
                                capture_output=True, timeout=15)
        output = result.stdout + result.stderr
        assert result.returncode == 2, (name, result.returncode, output)
        assert diagnostic in output, (name, output)
        assert "step 1/6" not in output, (name, output)
        assert snapshot() == original, (name, "existing title changed")
        print(f"PASS {name}: exit=2 before build; titles unchanged")
        passed += 1

    with tempfile.TemporaryDirectory(dir=scratch) as temporary:
        work = Path(temporary)
        data = work / "boot-data.bin"
        data.write_bytes(bytes((1, 2, 3, 4)))
        environment["RCOMP_XEX_BOOT_DATA_FILE"] = str(data)
        reject("invalid-services-mode", {"RCOMP_XEX_SERVICES": "2"}, "must be 0 or 1")
        reject("services-requires-xex", {}, "requires --xex", ())
        for mode in ("PACKAGED_BOOT", "POOL_RTL", "MODULES", "CALENDAR_THREADS", "BOOT_REHEARSAL"):
            reject("conflict-" + mode, {"RCOMP_XEX_" + mode: "1"}, "mutually exclusive")
        reject("missing-data-setting", {"RCOMP_XEX_BOOT_DATA_FILE": ""}, "must name")
        reject("missing-data-file", {"RCOMP_XEX_BOOT_DATA_FILE": str(work / "absent.bin")}, "must name")
        wrong = work / "wrong.bin"
        wrong.write_bytes(bytes((1, 2, 3, 5)))
        reject("wrong-data-content", {"RCOMP_XEX_BOOT_DATA_FILE": str(wrong)}, "exactly 01 02 03 04")
        reject("missing-xex", {"RCOMP_XEX_FILE": str(work / "absent.xex")}, "missing ")
        partial = work / "archives"
        archives = ("librcomp_xex_selftest.a", "librcomp_xex_generated.a",
                    "runtime/librcomp_runtime.a", "runtime/librcomp_runtime_video.a",
                    "platform/librcomp_platform_input.a")
        for name in archives:
            target = partial / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.xex_build / name, target)
        for name in archives:
            target = partial / name
            target.unlink()
            reject("missing-" + target.name, {"RCOMP_XEX_BUILD": str(partial)}, "missing " + str(target))
            shutil.copyfile(args.xex_build / name, target)
    print(f"PASS services title prerequisites {passed}/{passed}")


if __name__ == "__main__":
    main()
