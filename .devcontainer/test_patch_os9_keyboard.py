#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("patch-os9-keyboard.py")
SPEC = importlib.util.spec_from_file_location("patch_os9_keyboard", SCRIPT)
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class Os9KeyboardPatchTests(unittest.TestCase):
    """Exercise the actual downloaded image and reject unrecognized inputs."""

    @classmethod
    def setUpClass(cls):
        """Load the original system image once for the image-level checks."""
        media = os.environ.get("ECONOPET_MEDIA_DIR")
        if not media:
            raise unittest.SkipTest("ECONOPET_MEDIA_DIR is not set")
        cls.source = Path(media) / "disks/superpet/os9/os9-systemdisk-b.d80"
        if not cls.source.is_file():
            raise unittest.SkipTest("OS-9 system disk is not installed")
        cls.image = cls.source.read_bytes()

    def test_patch_preserves_disk_and_installs_key_mappings(self):
        """Only PRG payload bytes change, with native codes at graphics positions."""
        patched = PATCH.patch_disk(self.image)
        positions = PATCH.file_positions(self.image)
        allowed = set(positions)
        self.assertEqual(len(patched), len(self.image))
        self.assertTrue(all(a == b for i, (a, b) in enumerate(zip(self.image, patched))
                            if i not in allowed))
        original_loader = bytes(self.image[i] for i in positions)
        loader = bytes(patched[i] for i in positions)
        changed = {i for i, (a, b) in enumerate(zip(original_loader, loader)) if a != b}
        expected = set(range(0x970, 0xa60)) | {0x8ae, 0x8b0, 0x8b4, 0x8b6, 0x8bf, 0x8c3, 0x8c5}
        self.assertTrue(changed)
        self.assertLessEqual(changed, expected)

        def key(row, bit):
            return tuple(loader[0x970 + mode * 80 + row * 8 + bit] for mode in range(3))

        self.assertEqual(key(4, 0), (ord("a"), ord("A"), 1))
        self.assertEqual(key(0, 0)[:2], (ord("!"), ord("1")))
        self.assertEqual(key(1, 0)[:2], (ord('"'), ord("2")))
        self.assertEqual(key(9, 7)[:2], (ord("="), ord("-")))
        self.assertEqual(key(7, 4)[:2], (ord("?"), ord("/")))
        self.assertEqual(key(6, 6), (ord("1"), 0x81, 0x8c))
        self.assertEqual(key(1, 5), (9, 9, 9))
        self.assertEqual(key(9, 5), (0x7f, 0x7f, 0x7f))
        self.assertEqual(key(0, 5), (ord("_"), ord("_"), 0x1f))
        self.assertEqual(key(8, 0), (0xff, 0xff, 0xff))
        self.assertEqual(key(9, 0), (0xff, 0xff, 0xff))
        self.assertEqual([loader[i] for i in (0x8ae, 0x8b0, 0x8b4, 0x8b6, 0x8bf, 0x8c3, 0x8c5)],
                         [0x21, 0x68, 0x68, 0x68, 0x69, 0x69, 0x69])

    def test_invalid_source_preserves_existing_output(self):
        """Reject unexpected images before replacing a previous output."""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input.d80"
            output = Path(directory) / "output.d80"
            damaged = bytearray(self.image)
            damaged[0] ^= 1
            source.write_bytes(damaged)
            output.write_bytes(b"existing")
            result = subprocess.run([sys.executable, SCRIPT, source, output],
                                    capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unexpected OS-9 system disk checksum", result.stderr)
            self.assertEqual(output.read_bytes(), b"existing")
            self.assertEqual(source.read_bytes(), damaged)

    def test_command_writes_separate_disk(self):
        """Publish the complete patched image without changing the source."""
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "graphics.d80"
            output.write_bytes(b"old output")
            subprocess.run([sys.executable, SCRIPT, self.source, output], check=True)
            self.assertEqual(output.read_bytes(), PATCH.patch_disk(self.image))
            self.assertEqual(output.stat().st_mode & 0o777, self.source.stat().st_mode & 0o777)
            self.assertEqual(self.source.read_bytes(), self.image)

    def test_rejects_source_as_output(self):
        """Never overwrite the original image, even through the command line."""
        result = subprocess.run([sys.executable, SCRIPT, self.source, self.source],
                                capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("output must differ", result.stderr)
        self.assertEqual(self.source.read_bytes(), self.image)


if __name__ == "__main__":
    unittest.main()
