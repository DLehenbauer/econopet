#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Merge Commodore D64 directory entries and file chains into a formatted D80.

import argparse
import subprocess
from pathlib import Path


SUPPORTED_FILE_TYPES = {1, 2, 3}  # SEQ, PRG, USR
FILE_TYPE_NAMES = {
    0: "DEL",
    1: "SEQ",
    2: "PRG",
    3: "USR",
    4: "REL",
    5: "reserved type 5",
    6: "reserved type 6",
    7: "reserved type 7",
}


def sectors_per_track(track):
    if track <= 39:
        return 29
    if track <= 53:
        return 27
    if track <= 64:
        return 25
    if track <= 77:
        return 23
    raise ValueError(f"invalid D80 track {track}")


def d64_sectors_per_track(track):
    if track <= 17:
        return 21
    if track <= 24:
        return 19
    if track <= 30:
        return 18
    if track <= 35:
        return 17
    raise ValueError(f"invalid D64 track {track}")


def sector_offset(track, sector, sectors):
    if sector >= sectors(track):
        raise ValueError(f"invalid sector {track}/{sector}")
    return (sum(sectors(number) for number in range(1, track)) + sector) * 256


def read_sector(image, track, sector, sectors):
    offset = sector_offset(track, sector, sectors)
    return image[offset:offset + 256]


def source_entries(image):
    track, sector = 18, 1
    seen = set()
    while track:
        if (track, sector) in seen:
            raise ValueError("D64 directory has a circular sector chain")
        seen.add((track, sector))
        directory = read_sector(image, track, sector, d64_sectors_per_track)
        for index in range(8):
            offset = index * 32
            entry = directory[offset:offset + 32]
            file_type = entry[2] & 0x07
            if file_type and file_type not in SUPPORTED_FILE_TYPES:
                raise ValueError(
                    f"unsupported D64 file type: {FILE_TYPE_NAMES[file_type]}"
                )
            if entry[2] & 0x80 and file_type:
                yield entry
        track, sector = directory[0], directory[1]


class D80:
    def __init__(self, path, name):
        self.path = path
        subprocess.run(
            ["c1541", "-format", f"{name},00", "d80", str(path)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        self.image = bytearray(path.read_bytes())
        if len(self.image) != 533248:
            raise ValueError("c1541 did not create a standard D80 image")
        self.directory_track = 39
        self.directory_sector = 1
        self.directory_offset = 0

    def offset(self, track, sector):
        return sector_offset(track, sector, sectors_per_track)

    def sector(self, track, sector):
        offset = self.offset(track, sector)
        return self.image[offset:offset + 256]

    def write_sector(self, track, sector, data):
        if len(data) != 256:
            raise ValueError("disk sectors must be 256 bytes")
        offset = self.offset(track, sector)
        self.image[offset:offset + 256] = data

    def bam_entry(self, track):
        if track <= 50:
            return 38, 0, 6 + (track - 1) * 5
        return 38, 3, 6 + (track - 51) * 5

    def is_free(self, track, sector):
        bam_track, bam_sector, entry = self.bam_entry(track)
        offset = self.offset(bam_track, bam_sector) + entry + 1 + sector // 8
        return bool(self.image[offset] & (1 << (sector % 8)))

    def allocate(self):
        for track in range(1, 78):
            if track in (38, 39):
                continue
            for sector in range(sectors_per_track(track)):
                if self.is_free(track, sector):
                    bam_track, bam_sector, entry = self.bam_entry(track)
                    offset = self.offset(bam_track, bam_sector) + entry
                    self.image[offset] -= 1
                    self.image[offset + 1 + sector // 8] &= ~(1 << (sector % 8))
                    return track, sector
        raise ValueError("D80 has no free sectors")

    def add_directory_entry(self, entry):
        if self.directory_offset == 256:
            next_track, next_sector = self.allocate()
            block = bytearray(self.sector(self.directory_track, self.directory_sector))
            block[0:2] = bytes((next_track, next_sector))
            self.write_sector(self.directory_track, self.directory_sector, block)
            self.directory_track, self.directory_sector = next_track, next_sector
            self.directory_offset = 0
            self.write_sector(next_track, next_sector, bytearray(256))
        block = bytearray(self.sector(self.directory_track, self.directory_sector))
        entry_start = self.directory_offset
        if entry_start == 0:
            block[2:32] = entry[2:32]
        else:
            block[entry_start:entry_start + 32] = entry
        self.write_sector(self.directory_track, self.directory_sector, block)
        self.directory_offset += 32

    def add_file(self, entry, source):
        track, sector = entry[3], entry[4]
        source_sectors = []
        seen = set()
        while track:
            if (track, sector) in seen:
                raise ValueError("D64 file has a circular sector chain")
            seen.add((track, sector))
            block = read_sector(source, track, sector, d64_sectors_per_track)
            source_sectors.append(block)
            track, sector = block[0], block[1]

        destination_sectors = [self.allocate() for _ in source_sectors]
        for index, block in enumerate(source_sectors):
            destination = bytearray(256)
            destination[2:] = block[2:]
            if index + 1 < len(destination_sectors):
                destination[0:2] = bytes(destination_sectors[index + 1])
            else:
                destination[0:2] = block[0:2]
            self.write_sector(*destination_sectors[index], destination)

        directory_entry = bytearray(entry)
        directory_entry[3:5] = bytes(destination_sectors[0])
        self.add_directory_entry(directory_entry)

    def save(self):
        self.path.write_bytes(self.image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("sources", type=Path, nargs="+")
    parser.add_argument("--name", default="WATERLOO2")
    args = parser.parse_args()

    sources = []
    for source_path in args.sources:
        source = source_path.read_bytes()
        if len(source) != 174848:
            raise ValueError(f"{source_path} is not a standard D64 image")
        sources.append((source, list(source_entries(source))))

    destination = D80(args.output, args.name)
    for source, entries in sources:
        for entry in entries:
            destination.add_file(entry, source)
    destination.save()


if __name__ == "__main__":
    main()