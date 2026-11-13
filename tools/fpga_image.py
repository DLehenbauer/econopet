#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Create a bounded FPGA flash image with a versioned header and CRC-32."""

import argparse
import struct
import zlib
from pathlib import Path

MAGIC = b"EFP1"
VERSION = 1
HEADER = struct.Struct("<4sIII")


def build_image(bitstream, capacity):
    """Return the header and raw bitstream, rejecting invalid partition sizes."""
    if not bitstream or len(bitstream) > capacity - HEADER.size:
        raise ValueError(f"bitstream size {len(bitstream)} exceeds capacity or is empty")
    return HEADER.pack(MAGIC, VERSION, len(bitstream), zlib.crc32(bitstream)) + bitstream


def main():
    """Generate an image using the partition size supplied by the flash layout."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--capacity", type=int, required=True)
    args = parser.parse_args()
    try:
        image = build_image(args.input.read_bytes(), args.capacity)
        args.output.write_bytes(image)
    except (OSError, ValueError) as error:
        parser.exit(1, f"FPGA image: {error}\n")


if __name__ == "__main__":
    main()
