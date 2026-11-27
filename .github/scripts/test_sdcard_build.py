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

    def test_prg_validation_accepts_custom_bounds_and_rejects_malformed_files(self):
        """Validate generic PRGs independently of Drive Test's base-RAM window."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prg = root / "custom.prg"
            script = root / "check.cmake"
            script.write_text(
                f'include("{ROOT / "cmake/ValidatePrg.cmake"}")\n'
                f'validate_prg("{prg}" 0x1200 0x1240)\n',
                encoding="ascii",
            )

            # Exercise little-endian decoding, inclusive start, and exclusive end with a custom window.
            cases = (
                ("valid", b"\x34\x12\x60", ""),
                ("exact window", b"\x00\x12" + bytes(0x40), ""),
                ("missing", None, "Required PRG file is missing"),
                ("empty", b"", "missing its load address or payload"),
                ("partial header", b"\x34", "missing its load address or payload"),
                ("no payload", b"\x34\x12", "missing its load address or payload"),
                ("below window", b"\xff\x11\x60", "loads 4607..4607"),
                ("past window", b"\x00\x12" + bytes(0x41), "loads 4608..4672"),
            )
            for name, contents, error in cases:
                with self.subTest(name=name):
                    if contents is None:
                        prg.unlink()
                    else:
                        prg.write_bytes(contents)
                    result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, not error, result.stdout + result.stderr)
                    if error:
                        self.assertIn(error, " ".join((result.stdout + result.stderr).split()))

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
            for name in ("rom", "disk"):
                (media / name).mkdir(parents=True)
            drive_prg = root / "drive-test-assets/qualification.prg"
            drive_prg.parent.mkdir(parents=True)
            drive_prg.write_bytes(b"\x01\x04\x60")
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
                source / "sdcard/prg/README.md": "prg/README.md",
                source / "sdcard/test/README.md": "test/README.md",
                media / "rom/basic.bin": "rom/basic.bin",
                media / "disk/test.d64": "disk/test.d64",
                roms / "edit.bin": "rom/edit.bin",
                drive_prg: "test/drive-test.prg",
            }
            for path in assets:
                if path == drive_prg:
                    path.write_bytes(b"\x01\x04\x60")
                else:
                    path.write_bytes(b"old")
            stale = media / "disk/removed.d64"
            stale.write_bytes(b"remove me")
            removed_rom = roms / "removed.bin"
            removed_rom.write_bytes(b"remove me")
            configure = ["cmake", "-S", str(source / "sdcard"), "-B", str(build / "sdcard"),
                         "-G", "Ninja", f"-DECONOPET_MEDIA_DIR={media}",
                         f"-DDRIVE_TEST_PRG={drive_prg}",
                         f"-DFIRMWARE_UF2={firmware}", f"-DFPGA_BITSTREAM={fpga}"]
            missing_bootloader = subprocess.run(configure, capture_output=True, text=True)
            self.assertNotEqual(missing_bootloader.returncode, 0)
            self.assertIn("BOOTLOADER_UF2 must be defined", missing_bootloader.stderr)
            subprocess.run(configure + [f"-DBOOTLOADER_UF2={bootloader}"],
                           check=True, capture_output=True)
            command = ["cmake", "--build", str(build / "sdcard")]
            packaged = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(packaged.returncode, 0, packaged.stdout + packaged.stderr)

            missing_contents = drive_prg.read_bytes()
            drive_prg.unlink()
            missing_asset = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(missing_asset.returncode, 0)
            self.assertIn("Required PRG file is missing",
                          missing_asset.stdout + missing_asset.stderr)

            # The staged RUN program must start at BASIC's actual $0401 program pointer.
            drive_prg.write_bytes(b"\x01\x08\x60")
            wrong_start = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(wrong_start.returncode, 0)
            self.assertIn("PRG must load at 1025", wrong_start.stdout + wrong_start.stderr)

            # A PRG whose last byte reaches $7FEF (32751) fits below the $7FF0 mailbox, while $7FF0 (32752) fails.
            drive_prg.write_bytes(b"\x01\x04" + bytes(0x7FF0 - 0x0401))
            exact_fit = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(exact_fit.returncode, 0, exact_fit.stdout + exact_fit.stderr)

            drive_prg.write_bytes(b"\x01\x04" + bytes(0x7FF1 - 0x0401))
            overflow = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(overflow.returncode, 0)
            self.assertIn("loads 1025..32752", " ".join((overflow.stdout + overflow.stderr).split()))
            drive_prg.write_bytes(missing_contents)

            # Keep source timestamps older than outputs to catch timestamp-only refresh.
            for path in assets:
                if path == drive_prg:
                    path.write_bytes(b"\x01\x04\xea")
                else:
                    path.write_bytes(b"new")
                shutil.copystat(firmware, path)
            stale.unlink()
            removed_rom.unlink()
            (roms / "late.bin").write_bytes(b"late")
            (roms / "menu.rom").write_bytes(b"not packaged")
            fpga.write_bytes(b"updated fpga")
            bootloader.write_bytes(b"updated bootloader")
            stale_fpga = build / "sdcard/sdcard_root/fpga"
            stale_fpga.mkdir()
            (stale_fpga / "old.bin").write_bytes(b"stale")

            # Reject same-name ROMs regardless of contents without clearing the previous staging tree.
            conflicting_rom = media / "rom/edit.bin"
            for contents in ((roms / "edit.bin").read_bytes(), b"conflicting"):
                conflicting_rom.write_bytes(contents)
                shutil.copystat(roms / "edit.bin", conflicting_rom)
                conflict = subprocess.run(command, capture_output=True, text=True)
                self.assertNotEqual(conflict.returncode, 0)
                output = " ".join((conflict.stdout + conflict.stderr).split())
                self.assertIn("ROM filename conflict", output)
                self.assertIn(str(roms / "edit.bin"), output)
                self.assertIn(str(conflicting_rom), output)
                self.assertEqual((stale_fpga / "old.bin").read_bytes(), b"stale")
            conflicting_rom.unlink()

            # FAT32 treats generated and media ROM names as equal regardless of letter case.
            case_conflict = media / "rom/EDIT.BIN"
            case_conflict.write_bytes(b"case conflict")
            case_variant = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(case_variant.returncode, 0)
            self.assertIn("ROM filename conflict", case_variant.stdout + case_variant.stderr)
            self.assertIn(str(case_conflict), case_variant.stdout + case_variant.stderr)
            self.assertEqual((stale_fpga / "old.bin").read_bytes(), b"stale")
            case_conflict.unlink()

            # Case variants within either source tree also collide on FAT32.
            media_case_conflict = media / "rom/BASIC.BIN"
            media_case_conflict.write_bytes(b"media case conflict")
            media_case_variant = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(media_case_variant.returncode, 0)
            self.assertIn("ROM filename conflict", media_case_variant.stdout + media_case_variant.stderr)
            self.assertEqual((stale_fpga / "old.bin").read_bytes(), b"stale")
            media_case_conflict.unlink()

            generated_case_conflict = roms / "EDIT.bin"
            generated_case_conflict.write_bytes(b"generated case conflict")
            generated_case_variant = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(generated_case_variant.returncode, 0)
            self.assertIn("ROM filename conflict", generated_case_variant.stdout + generated_case_variant.stderr)
            self.assertEqual((stale_fpga / "old.bin").read_bytes(), b"stale")
            generated_case_conflict.unlink()

            packaged = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(packaged.returncode, 0, packaged.stdout + packaged.stderr)

            with zipfile.ZipFile(next((build / "sdcard").glob("*.zip"))) as archive:
                contents = {name.removeprefix("./"): archive.read(name)
                            for name in archive.namelist() if not name.endswith("/")}
            for relative in assets.values():
                if relative == "test/drive-test.prg":
                    self.assertEqual(contents[relative], b"\x01\x04\xea", relative)
                else:
                    self.assertEqual(contents[relative], b"new", relative)
            self.assertNotIn("prg/drive-test.prg", contents)
            self.assertNotIn(f"test/{drive_prg.name}", contents)
            self.assertFalse(any(name.startswith("disk/drive-test/") for name in contents))
            self.assertNotIn("disk/removed.d64", contents)
            self.assertNotIn("rom/removed.bin", contents)
            self.assertEqual(contents["rom/late.bin"], b"late")
            self.assertNotIn("rom/menu.rom", contents)
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
