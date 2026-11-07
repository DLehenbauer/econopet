#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Exercise SD-card staging across builds without reconfiguring."""

import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile
import zlib
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
            (source / "tools").mkdir()
            shutil.copyfile(ROOT / "tools/fpga_image.py", source / "tools/fpga_image.py")
            shutil.copyfile(ROOT / "NOTICE.md", source / "NOTICE.md")
            media = root / "media"
            for name in ("roms", "disks"):
                (media / name).mkdir(parents=True)
            build = root / "build"
            roms = build / "rom" / "bin"
            roms.mkdir(parents=True)
            firmware = root / "firmware.uf2"
            fpga = root / "fpga.bin"
            bootloader = root / "custom-bootloader.uf2"
            bootloader.write_bytes(b"old bootloader")
            # Use a valid RP2040 UF2 with an application vector table.
            payload = struct.pack("<II", 0x20042000, 0x100001c1) + bytes(248)
            block = struct.pack("<8I", 0x0a324655, 0x9e5d5157, 0x2000,
                                0x10000100, 256, 0, 1, 0xe48bff56)
            firmware.write_bytes(block + payload + bytes(220) + struct.pack("<I", 0x0ab16f30))
            original_firmware = firmware.read_bytes()
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
            configure = ["cmake", "-S", str(source / "sdcard"), "-B", str(build / "sdcard"),
                         "-G", "Ninja", f"-DECONOPET_MEDIA_DIR={media}",
                         f"-DFIRMWARE_UF2={firmware}", f"-DFPGA_BITSTREAM={fpga}"]
            missing_bootloader = subprocess.run(configure, capture_output=True, text=True)
            self.assertNotEqual(missing_bootloader.returncode, 0)
            self.assertIn("BOOTLOADER_UF2 must be defined", missing_bootloader.stderr)
            subprocess.run(configure + [f"-DBOOTLOADER_UF2={bootloader}"],
                           check=True, capture_output=True)
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
            fpga.write_bytes(b"updated fpga")
            bootloader.write_bytes(b"updated bootloader")
            stale_fpga = build / "sdcard/sdcard_root/fpga"
            stale_fpga.mkdir()
            (stale_fpga / "old.bin").write_bytes(b"stale")
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
            self.assertFalse(any(name.startswith("fpga/") for name in contents))
            self.assertEqual(firmware.read_bytes(), original_firmware)
            self.assertEqual(contents["bootloader.uf2"], b"updated bootloader")
            self.assertNotIn(bootloader.name, contents)

            # Check every block against the existing bootloader's UF2 contract.
            combined = contents["firmware.uf2"]
            self.assertEqual(len(combined) % 512, 0)
            count = len(combined) // 512
            pages = {}
            for number in range(count):
                block = combined[number * 512:(number + 1) * 512]
                magic0, magic1, flags, address, size, index, total, family = struct.unpack_from("<8I", block)
                self.assertEqual((magic0, magic1, flags, size, index, total, family),
                                 (0x0a324655, 0x9e5d5157, 0x2000, 256, number, count, 0xe48bff56))
                self.assertEqual(struct.unpack_from("<I", block, 508)[0], 0x0ab16f30)
                self.assertEqual(address % 256, 0)
                self.assertTrue(0x10000000 <= address < 0x10ff0000)
                self.assertNotIn(address, pages)
                pages[address] = block[32:288]
            self.assertEqual(pages[0x10000100], payload)
            image = pages[0x10F3C000]
            magic, version, length, crc = struct.unpack_from("<4sIII", image)
            self.assertEqual((magic, version, length), (b"EFP1", 1, len(b"updated fpga")))
            self.assertEqual(image[16:16 + length], b"updated fpga")
            self.assertEqual(crc, zlib.crc32(b"updated fpga"))


if __name__ == "__main__":
    unittest.main()
