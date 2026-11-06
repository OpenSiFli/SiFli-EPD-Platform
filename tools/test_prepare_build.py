"""Check SDK patch application, idempotence, and preservation of local edits."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import prepare_build


ROOT = Path(__file__).resolve().parents[1]
FILES = ("drivers/Include/bf0_hal_lcdc.h", "drivers/hal/bf0_hal_lcdc.c")


class SDKPatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        self.sdk = self.repo / "SiFli-SDK"
        self.sdk.mkdir()
        subprocess.run(["git", "init", "--quiet"], cwd=self.sdk, check=True)
        subprocess.run(["git", "config", "core.autocrlf", "false"], cwd=self.sdk, check=True)
        for name in FILES:
            original = subprocess.run(["git", "show", f"HEAD:{name}"],
                                      cwd=ROOT / "SiFli-SDK", check=True,
                                      capture_output=True).stdout
            path = self.sdk / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(original)
        shutil.copytree(ROOT / "patches", self.repo / "patches")
        previous = prepare_build.REPO
        prepare_build.REPO = self.repo
        self.addCleanup(setattr, prepare_build, "REPO", previous)

    def snapshot(self):
        return [(self.sdk / name).read_bytes() for name in FILES]

    def test_apply_and_repeat_preserve_unrelated_edits(self):
        source = self.sdk / FILES[1]
        with source.open("ab") as stream:
            stream.write(b"\n/* unrelated local edit */\n")
        prepare_build.apply_sdk_patches()
        applied = self.snapshot()
        self.assertIn(b"HAL_LCDC_EPD_START_HOOK_SUPPORTED", applied[0])
        self.assertIn(b"if (!HAL_LCDC_EPD_StartHook(lcdc))", applied[1])
        self.assertTrue(applied[1].endswith(b"/* unrelated local edit */\n"))
        prepare_build.apply_sdk_patches()
        self.assertEqual(applied, self.snapshot())

    def test_windows_line_endings(self):
        subprocess.run(["git", "config", "core.autocrlf", "true"], cwd=self.sdk, check=True)
        for name in FILES:
            path = self.sdk / name
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        prepare_build.apply_sdk_patches()
        applied = self.snapshot()
        prepare_build.apply_sdk_patches()
        self.assertEqual(applied, self.snapshot())

    def test_conflict_does_not_apply_any_hunk(self):
        source = self.sdk / FILES[1]
        original = source.read_bytes()
        source.write_bytes(original.replace(
            b"            /* trigger lcdc */",
            b"            /* conflicting local start sequence */"))
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "SDK patch conflicts"):
            prepare_build.apply_sdk_patches()
        self.assertEqual(before, self.snapshot())

    def test_partial_application_is_reported(self):
        prepare_build.apply_sdk_patches()
        original = subprocess.run(["git", "show", f"HEAD:{FILES[1]}"],
                                  cwd=ROOT / "SiFli-SDK", check=True,
                                  capture_output=True).stdout
        (self.sdk / FILES[1]).write_bytes(original)
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "SDK patch conflicts"):
            prepare_build.apply_sdk_patches()
        self.assertEqual(before, self.snapshot())


if __name__ == "__main__":
    unittest.main()
