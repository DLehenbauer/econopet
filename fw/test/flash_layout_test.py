# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Check the firmware and packaged bootloader flash contract without hardware."""

import argparse
import re
import struct
import unittest
from pathlib import Path

FLASH_BASE = 0x10000000
FLASH_SIZE = 16 * 1024 * 1024
BOOTLOADER_SIZE = 64 * 1024
BOOTLOADER_START = FLASH_BASE + FLASH_SIZE - BOOTLOADER_SIZE
PAGE_SIZE = 256
SECTOR_SIZE = 4096
UF2_BLOCK_SIZE = 512
UF2_MAGIC_START = (0x0A324655, 0x9E5D5157)
UF2_MAGIC_END = 0x0AB16F30
UF2_FAMILY_ID_PRESENT = 0x2000
RP2040_FAMILY_ID = 0xE48BFF56
SRAM_BASE = 0x20000000
SRAM_END = 0x20042000


def read_uf2(path):
    """Return flash pages from an RP2040 UF2, rejecting invalid or overlapping blocks."""
    data = path.read_bytes()
    if not data or len(data) % UF2_BLOCK_SIZE:
        raise ValueError(f"{path}: invalid UF2 length")
    pages = {}
    count = len(data) // UF2_BLOCK_SIZE
    block_numbers = set()
    for offset in range(0, len(data), UF2_BLOCK_SIZE):
        magic0, magic1, flags, address, size, number, total, family = struct.unpack_from(
            "<8I", data, offset
        )
        end_magic = struct.unpack_from("<I", data, offset + 508)[0]
        if (
            (magic0, magic1) != UF2_MAGIC_START
            or end_magic != UF2_MAGIC_END
            or not flags & UF2_FAMILY_ID_PRESENT
            or family != RP2040_FAMILY_ID
            or size != PAGE_SIZE
            or address % PAGE_SIZE
            or total != count
            or number >= count
            or number != offset // UF2_BLOCK_SIZE
            or number in block_numbers
            or address in pages
        ):
            raise ValueError(f"{path}: invalid UF2 block at offset {offset}")
        block_numbers.add(number)
        pages[address] = data[offset + 32 : offset + 32 + size]
    return pages


def read_elf_segments(path):
    """Return file-backed ELF32 load segments using physical flash addresses."""
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError(f"{path}: expected little-endian ELF32")
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    if phentsize < 32 or phoff + phentsize * phnum > len(data):
        raise ValueError(f"{path}: invalid ELF program header table")
    segments = []
    for index in range(phnum):
        kind, offset, _, address, size, _, _, _ = struct.unpack_from(
            "<8I", data, phoff + index * phentsize
        )
        if kind == 1 and size:
            if offset + size > len(data):
                raise ValueError(f"{path}: truncated ELF load segment")
            segments.append((address, data[offset : offset + size]))
    return segments


class FlashLayoutTest(unittest.TestCase):
    """Verify SWD and UF2 images preserve the installed SD bootloader."""

    @classmethod
    def setUpClass(cls):
        """Load the actual firmware and the bootloader distributed with EconoPET."""
        cls.elf = read_elf_segments(ARGS.elf)
        cls.firmware = read_uf2(ARGS.firmware)
        cls.bootloader = read_uf2(ARGS.bootloader) if ARGS.bootloader is not None else None

    def test_partition_and_uf2_match_elf(self):
        """Check all flash load bytes and erase sectors stay below the FPGA partition."""
        self.assertIn(FLASH_BASE, self.firmware)
        self.assertIn(FLASH_BASE + PAGE_SIZE, self.firmware)
        for address, page in self.firmware.items():
            self.assertGreaterEqual(address, FLASH_BASE)
            self.assertLessEqual(address + len(page), ARGS.fpga_address)
        for address, data in self.elf:
            if FLASH_BASE <= address < FLASH_BASE + FLASH_SIZE:
                self.assertLessEqual(address + len(data), ARGS.fpga_address)
                for index, value in enumerate(data):
                    page_address = (address + index) & ~(PAGE_SIZE - 1)
                    self.assertIn(page_address, self.firmware)
                    self.assertEqual(value, self.firmware[page_address][(address + index) % PAGE_SIZE])

    def test_sdk_linker_region(self):
        """Check the real SDK linker region ends at the reserved partition."""
        text = ARGS.map.read_text()
        match = re.search(r"^FLASH\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)", text, re.MULTILINE)
        self.assertIsNotNone(match, "FLASH region missing from firmware link map")
        origin, length = (int(value, 16) for value in match.groups())
        self.assertEqual(origin, FLASH_BASE)
        self.assertEqual(origin + length, ARGS.fpga_address)

    def test_built_bootloader(self):
        """Check the built UF2 matches its ELF and shares firmware's stage-2 stub."""
        if ARGS.bootloader_elf is None:
            self.skipTest("built bootloader ELF was not supplied")
        expected_pages = set()
        for address, data in read_elf_segments(ARGS.bootloader_elf):
            if not FLASH_BASE <= address < FLASH_BASE + FLASH_SIZE:
                continue
            self.assertTrue(address == FLASH_BASE or address >= BOOTLOADER_START)
            self.assertLessEqual(address + len(data),
                                 FLASH_BASE + PAGE_SIZE if address == FLASH_BASE
                                 else FLASH_BASE + FLASH_SIZE)
            for index, value in enumerate(data):
                page_address = (address + index) & ~(PAGE_SIZE - 1)
                expected_pages.add(page_address)
                self.assertIn(page_address, self.bootloader)
                self.assertEqual(value, self.bootloader[page_address][(address + index) % PAGE_SIZE])
        # picotool may zero-pad the stage-2 erase sector, invalidating old firmware vectors.
        for address in set(self.bootloader) - expected_pages:
            self.assertTrue(FLASH_BASE + PAGE_SIZE <= address < FLASH_BASE + SECTOR_SIZE)
            self.assertEqual(self.bootloader[address], bytes(PAGE_SIZE))
        self.assertEqual(self.bootloader[FLASH_BASE], self.firmware[FLASH_BASE])

    def test_boot_chain(self):
        """Verify app vectors and stage-2 destination, plus stage 3 when supplied."""
        boot2 = self.firmware[FLASH_BASE]
        # Locate the boot-ROM return check and decode its PC-relative stage-3 address.
        check = b"\x01\xbc\x00\x28\x00\xd0\x00\x47"
        offset = boot2.find(check)
        self.assertNotEqual(offset, -1, "stage-2 reset branch not found")
        vector = offset + len(check)
        first, second, store, load, msr1, msr2, branch = struct.unpack_from("<7H", boot2, vector)
        self.assertEqual(first & 0xFF00, 0x4800)
        self.assertEqual(second & 0xFF00, 0x4900)
        self.assertEqual((store, load, msr1, msr2, branch), (0x6008, 0xC803, 0xF380, 0x8808, 0x4708))
        literal = ((vector + 4) & ~3) + (first & 0xFF) * 4
        self.assertEqual(struct.unpack_from("<I", boot2, literal)[0], BOOTLOADER_START)

        if self.bootloader is not None:
            self.assertIn(BOOTLOADER_START, self.bootloader)
            stage3_sp, stage3_pc = struct.unpack_from("<II", self.bootloader[BOOTLOADER_START])
            self.assertTrue(SRAM_BASE <= stage3_sp <= SRAM_END)
            self.assertTrue(stage3_pc & 1)
            self.assertTrue(BOOTLOADER_START <= (stage3_pc & ~1) < FLASH_BASE + FLASH_SIZE)
        app_sp, app_pc = struct.unpack_from("<II", self.firmware[FLASH_BASE + PAGE_SIZE])
        self.assertTrue(SRAM_BASE <= app_sp <= SRAM_END)
        self.assertTrue(app_pc & 1)
        self.assertTrue(FLASH_BASE + PAGE_SIZE + 0xC0 <= (app_pc & ~1) < BOOTLOADER_START)

    def test_combined_update(self):
        """Check the packaged UF2 contains exactly the firmware and FPGA image pages."""
        if ARGS.combined is None:
            self.skipTest("combined SD-card update was not supplied")
        combined = read_uf2(ARGS.combined)
        image = ARGS.fpga_image.read_bytes()
        self.assertTrue(image)
        self.assertLessEqual(ARGS.fpga_address + len(image), BOOTLOADER_START)
        expected = dict(self.firmware)
        for offset in range(0, len(image), PAGE_SIZE):
            address = ARGS.fpga_address + offset
            self.assertNotIn(address, expected)
            expected[address] = combined[address]
            chunk = image[offset:offset + PAGE_SIZE]
            self.assertEqual(combined[address][:len(chunk)], chunk)
        self.assertEqual(combined, expected)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--bootloader", type=Path)
    parser.add_argument("--bootloader-elf", type=Path)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--fpga-address", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--combined", type=Path)
    parser.add_argument("--fpga-image", type=Path)
    ARGS = parser.parse_args()
    if ARGS.bootloader_elf is not None and ARGS.bootloader is None:
        parser.error("--bootloader-elf requires --bootloader")
    if (ARGS.combined is None) != (ARGS.fpga_image is None):
        parser.error("--combined and --fpga-image must be supplied together")
    unittest.main(argv=[__file__])
