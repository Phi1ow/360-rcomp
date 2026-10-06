"""Original synthetic assembly inputs; no executable validity claim."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from m6_package_title import package_title


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rcomp assembly spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.linked = self.root / "linked artifacts"
        self.game = self.root / "title data"
        self.template = self.root / "template"
        self.out = self.root / "assembled title"
        self.image = self.root / "decoded image.xex"
        for directory in (self.linked, self.game / "sub dir", self.template / "sce_sys", self.template / "sce_module"):
            directory.mkdir(parents=True)
        self.image.write_bytes(b"ORIGINAL SYNTHETIC DECODED IMAGE")
        (self.linked / "eboot.bin").write_bytes(b"TEST INPUT: NOT A SELF")
        (self.linked / "eboot.elf").write_bytes(b"TEST INPUT: NOT AN ELF")
        (self.game / "sub dir" / "data.bin").write_bytes(bytes(range(256)))
        (self.game / "plain.xex").write_bytes(b"DATA FILE, NOT DECODED IMAGE")
        (self.template / "sce_module/libc.prx").write_bytes(b"TEST INPUT: NOT A PRX")
        self.param = {"titleId": "PPSA88360", "contentId": "UP9000-PPSA88360_00-RCOMPTEST0000001", "conceptId": "88360"}
        self.write_param()

    def write_param(self):
        (self.template / "sce_sys/param.json").write_text(json.dumps(self.param), encoding="utf-8")

    def assemble(self):
        return package_title(self.linked, self.image, self.game, self.template, self.out)

    def test_distinct_image_data_and_destination_with_hashes(self):
        result = self.assemble()
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["ps5_executable_validity"], "NOT TESTED")
        self.assertEqual(result["ps5_execution"], "NOT TESTED")
        self.assertEqual((self.out / "image/plain.xex").read_bytes(), self.image.read_bytes())
        self.assertEqual((self.out / "game/plain.xex").read_bytes(), b"DATA FILE, NOT DECODED IMAGE")
        self.assertFalse((self.out / "ASSEMBLY_INCOMPLETE").exists())
        for name, record in result["files"].items():
            data = (self.out / name).read_bytes()
            self.assertEqual(record["bytes"], len(data))
            self.assertEqual(record["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(json.loads((self.out / "ASSEMBLY.json").read_text()), result)
        self.assertIn("game/sub dir/data.bin", (self.out / "manifest.sha256").read_text())

    def test_existing_destination_never_overwritten(self):
        self.out.mkdir()
        sentinel = self.out / "user.txt"; sentinel.write_text("preserve")
        with self.assertRaisesRegex(ValueError, "already exists"): self.assemble()
        self.assertEqual(sentinel.read_text(), "preserve")

    def test_missing_template_component_before_output_creation(self):
        (self.template / "sce_module/libc.prx").unlink()
        with self.assertRaisesRegex(ValueError, "missing regular file"): self.assemble()
        self.assertFalse(self.out.exists())

    def test_mismatching_identity(self):
        self.param["contentId"] = "UP9000-PPSA99999_00-RCOMPTEST00000001"; self.write_param()
        with self.assertRaisesRegex(ValueError, "contentId"): self.assemble()
        self.assertFalse(self.out.exists())

    def test_empty_linked_artifact(self):
        (self.linked / "eboot.bin").write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "empty required"): self.assemble()

    def test_destination_inside_input_refused(self):
        self.out = self.game / "recursive output"
        with self.assertRaisesRegex(ValueError, "overlaps"): self.assemble()

    def test_output_in_git_worktree_outside_build_refused(self):
        (self.root / ".git").mkdir()
        with self.assertRaisesRegex(ValueError, "outside git"): self.assemble()
        self.out = self.root / "build" / "title"; self.assemble()

    def test_symlink_input_refused(self):
        link = self.game / "linked-file"
        try: link.symlink_to(self.image)
        except OSError as error: self.skipTest("host does not permit symlinks: " + str(error))
        with self.assertRaisesRegex(ValueError, "symlink/junction"): self.assemble()
        self.assertFalse(self.out.exists())

    def test_copy_failure_keeps_incomplete_marker_without_success_manifest(self):
        with mock.patch("m6_package_title.shutil.copyfileobj", side_effect=PermissionError("TESTDOUBLE copy access failure")):
            with self.assertRaises(PermissionError): self.assemble()
        self.assertTrue((self.out / "ASSEMBLY_INCOMPLETE").exists())
        self.assertFalse((self.out / "ASSEMBLY.json").exists())

    def test_cli_paths_with_spaces(self):
        proc = subprocess.run([sys.executable, str(ROOT / "tools/m6_package_title.py"),
            "--linked", str(self.linked), "--image", str(self.image), "--game-root", str(self.game),
            "--template", str(self.template), "--out", str(self.out)], capture_output=True, text=True, timeout=20)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("NOT TESTED", proc.stdout)


if __name__ == "__main__": unittest.main()
