#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Create a graphics-keyboard Super-OS/9 disk from the downloaded system disk.

import argparse
import hashlib
import os
import tempfile
from pathlib import Path


SOURCE_MD5 = "a772ba7ad5d2f6a635a092f80f80d45c"
LOADER_MD5 = "8a94e77a89203146a2e5903af060e84b"
SECTOR_SIZE = 256
DIRECTORY_TRACK = 39
DIRECTORY_SECTOR = 1
DIRECTORY_ENTRIES = 8
ENTRY_SIZE = 32
PRG_CLOSED = 0x82
LOADER_NAME = b"os9" + b"\xa0" * 13
TABLE_OFFSET = 0x970
TABLE_SIZE = 80
NO_KEY = 0xff

# Business coordinates are KSCAN-major, with KIN in ascending bit order.
BUSINESS = (
    ("2", "5", "8", "-", "KP8", "RIGHT", None, None),
    ("1", "4", "7", "0", "KP7", "UP", None, "KP9"),
    ("ESC", "S", "F", "H", "]", "K", ";", "KP5"),
    ("A", "D", "G", "J", "RETURN", "L", "@", "KP6"),
    ("TAB", "W", "R", "Y", "\\", "I", "P", "DELETE"),
    ("Q", "E", "T", "U", "DOWN", "O", "[", "KP4"),
    (None, "C", "B", ".", "KP.", None, None, "KP3"),
    ("Z", "V", "N", ",", "KP0", None, "REPEAT", "KP2"),
    (None, "X", "SPACE", "M", "HOME", None, "/", "KP1"),
    ("BACK", "3", "6", "9", "STOP", ":", None, None),
)

# A leading "!" reverses the native unshifted/shifted pair to match the
# graphics keycap. Shift and RVS are handled separately by the scanner.
GRAPHICS = (
    ("!1", "!3", "!5", "!6", "!8", "BACK", "HOME", "RIGHT"),
    ("!2", "!4", "!7", "\\", "!9", "TAB", "DOWN", "DELETE"),
    ("Q", "E", "T", "U", "O", "UP", "KP7", "KP9"),
    ("W", "R", "Y", "I", "P", None, "KP8", "/"),
    ("A", "D", "G", "J", "L", None, "KP4", "KP6"),
    ("S", "F", "H", "K", ":", None, "KP5", "!:"),
    ("Z", "C", "B", "M", ";", "RETURN", "KP1", "KP3"),
    ("X", "V", "N", ",", "!/", None, "KP2", "!;"),
    (None, "@", "]", None, "!.", None, "KP0", "-"),
    (None, "[", "SPACE", "!,", "STOP", "REPEAT", "KP.", "!-"),
)

MODIFIER_PATCHES = (
    (0x08ae, 0x41, 0x21),
    (0x08b0, 0x66, 0x68),
    (0x08b4, 0x66, 0x68),
    (0x08b6, 0x66, 0x68),
    (0x08bf, 0x68, 0x69),
    (0x08c3, 0x68, 0x69),
    (0x08c5, 0x68, 0x69),
)


def md5(data):
    """Identify a known historical media payload, not a security signature."""
    return hashlib.md5(data, usedforsecurity=False).hexdigest()


def sector_offset(track, sector):
    """Resolve and validate one physical sector of a standard D80."""
    if not 1 <= track <= 77:
        raise ValueError(f"invalid D80 track {track}")
    sectors = lambda t: 29 if t <= 39 else 27 if t <= 53 else 25 if t <= 64 else 23
    if not 0 <= sector < sectors(track):
        raise ValueError(f"invalid D80 sector {track}/{sector}")
    return (sum(sectors(t) for t in range(1, track)) + sector) * SECTOR_SIZE


def file_positions(image):
    """Find the closed os9 PRG and return its physical payload positions."""
    directory = (DIRECTORY_TRACK, DIRECTORY_SECTOR)
    seen = set()
    starts = []
    while directory[0]:
        if directory in seen:
            raise ValueError("cyclic D80 directory")
        seen.add(directory)
        offset = sector_offset(*directory)
        block = image[offset:offset + SECTOR_SIZE]
        for index in range(DIRECTORY_ENTRIES):
            entry = block[2 + index * ENTRY_SIZE:2 + index * ENTRY_SIZE + 30]
            if entry[3:19] == LOADER_NAME:
                if entry[0] != PRG_CLOSED:
                    raise ValueError("os9 is not a closed PRG")
                starts.append((entry[1], entry[2]))
        directory = (block[0], block[1])
    if len(starts) != 1:
        raise ValueError("expected exactly one os9 PRG")

    positions = []
    current = starts[0]
    seen.clear()
    while current[0]:
        if current in seen:
            raise ValueError("cyclic os9 PRG")
        seen.add(current)
        offset = sector_offset(*current)
        next_track, next_sector = image[offset:offset + 2]
        if not next_track and next_sector == 0:
            raise ValueError("invalid final PRG sector length")
        length = SECTOR_SIZE - 2 if next_track else next_sector - 1
        positions.extend(range(offset + 2, offset + 2 + length))
        current = (next_track, next_sector)
    return positions


def patch_loader(original):
    """Move the native key translations and modifier tests to graphics positions."""
    if md5(original) != LOADER_MD5:
        raise ValueError("unexpected os9 PRG checksum")
    result = bytearray(original)
    for offset, old, new in MODIFIER_PATCHES:
        if result[offset] != old:
            raise ValueError(f"unexpected modifier byte at PRG offset {offset:#x}")
        result[offset] = new

    business = {
        key: row * 8 + bit
        for row, keys in enumerate(BUSINESS)
        for bit, key in enumerate(keys)
        if key is not None
    }
    tables = [bytearray([NO_KEY] * TABLE_SIZE) for _ in range(3)]
    for row, keys in enumerate(GRAPHICS):
        for bit, key in enumerate(keys):
            if key is None:
                continue
            swapped = key.startswith("!")
            name = key[1:] if swapped else key
            source = business[name]
            target = row * 8 + bit
            for mode in range(3):
                source_mode = 1 - mode if swapped and mode < 2 else mode
                tables[mode][target] = original[TABLE_OFFSET + source_mode * TABLE_SIZE + source]
    result[TABLE_OFFSET:TABLE_OFFSET + 3 * TABLE_SIZE] = b"".join(tables)
    return bytes(result)


def patch_disk(image):
    """Patch only the loader payload, preserving the disk's REL file and metadata."""
    if md5(image) != SOURCE_MD5:
        raise ValueError("unexpected OS-9 system disk checksum")
    positions = file_positions(image)
    original = bytes(image[position] for position in positions)
    patched = patch_loader(original)
    if len(patched) != len(positions):
        raise ValueError("patched os9 PRG changed length")
    result = bytearray(image)
    for position, value in zip(positions, patched, strict=True):
        result[position] = value
    return bytes(result)


def main():
    """Write a separate patched D80 only after the complete input is verified."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        raise ValueError("output must differ from the source disk")
    patched = patch_disk(args.source.read_bytes())
    with tempfile.NamedTemporaryFile(dir=args.output.parent, delete=False) as temporary:
        try:
            temporary.write(patched)
            temporary.close()
            os.chmod(temporary.name, args.source.stat().st_mode & 0o777)
            os.replace(temporary.name, args.output)
        finally:
            if os.path.exists(temporary.name):
                os.unlink(temporary.name)


if __name__ == "__main__":
    main()
