#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Exercise SD-card staging across builds without reconfiguring."""

import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class SdcardBuildTests(unittest.TestCase):
    """Build packages with small stand-ins for firmware, gateware, and media."""

    def test_incremental_package_refreshes_and_removes_assets(self):
        """Refresh all asset classes and discover ROM changes at build time."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            shutil.copytree(ROOT / "sdcard", source / "sdcard")
            shutil.copytree(ROOT / "cmake", source / "cmake")
            shutil.copyfile(ROOT / "NOTICE.md", source / "NOTICE.md")
            media = root / "media"
            for name in ("roms", "disks"):
                (media / name).mkdir(parents=True)
            build = root / "build"
            roms = build / "rom" / "bin"
            roms.mkdir(parents=True)
            firmware = root / "firmware.uf2"
            fpga = root / "fpga.bin"
            firmware.write_bytes(b"firmware")
            fpga.write_bytes(b"fpga")
            assets = {
                source / "sdcard/config.yaml": "config.yaml",
                source / "sdcard/README.md": "README.md",
                source / "NOTICE.md": "NOTICE.md",
                source / "sdcard/prgs/README.md": "prgs/README.md",
                source / "sdcard/ukm/us.bin": "ukm/us.bin",
                media / "roms/basic.bin": "roms/basic.bin",
                media / "disks/test.d64": "disks/test.d64",
                roms / "edit.bin": "roms/edit.bin",
            }
            for path in assets:
                path.write_bytes(b"old")
            stale = media / "disks/removed.d64"
            stale.write_bytes(b"remove me")
            removed_rom = roms / "removed.bin"
            removed_rom.write_bytes(b"remove me")
            removed_keymap = source / "sdcard/ukm/removed.bin"
            removed_keymap.write_bytes(b"remove me")
            subprocess.run(
                ["cmake", "-S", str(source / "sdcard"), "-B", str(build / "sdcard"),
                 "-G", "Ninja", f"-DECONOPET_MEDIA_DIR={media}",
                 f"-DFIRMWARE_UF2={firmware}", f"-DFPGA_BITSTREAM={fpga}"],
                check=True, capture_output=True,
            )
            command = ["cmake", "--build", str(build / "sdcard")]
            subprocess.run(command, check=True, capture_output=True)

            # Keep source timestamps older than outputs to catch timestamp-only refresh.
            for path in assets:
                path.write_bytes(b"new")
                shutil.copystat(firmware, path)
            stale.unlink()
            removed_rom.unlink()
            removed_keymap.unlink()
            (roms / "late.bin").write_bytes(b"late")
            (roms / "menu.rom").write_bytes(b"not packaged")
            (media / "roms/edit.bin").write_bytes(b"bad")
            shutil.copystat(roms / "edit.bin", media / "roms/edit.bin")
            subprocess.run(command, check=True, capture_output=True)

            with zipfile.ZipFile(next((build / "sdcard").glob("*.zip"))) as archive:
                contents = {name.removeprefix("./"): archive.read(name)
                            for name in archive.namelist() if not name.endswith("/")}
            for relative in assets.values():
                self.assertEqual(contents[relative], b"new", relative)
            self.assertNotIn("disks/removed.d64", contents)
            self.assertNotIn("roms/removed.bin", contents)
            self.assertNotIn("ukm/removed.bin", contents)
            self.assertEqual(contents["roms/late.bin"], b"late")
            self.assertNotIn("roms/menu.rom", contents)
            self.assertEqual(contents["firmware.uf2"], b"firmware")
            self.assertEqual(contents["fpga/EconoPET.hex.bin"], b"fpga")


if __name__ == "__main__":
    unittest.main()
