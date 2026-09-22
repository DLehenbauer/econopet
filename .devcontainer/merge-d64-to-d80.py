#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Merge the two Waterloo language D64 images into a D80 using VICE c1541.

import argparse
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


C1541_VERSION = "c1541 (VICE 3.9)"
D64_SIZE = 174848
SUPPORTED_FILE_TYPES = {"prg": ".P00", "seq": ".S00"}
DISK_NAME = re.compile(r"^[A-Z0-9][A-Z0-9 ]{0,15}$")
DIRECTORY_HEADER = re.compile(
    r'^\s*0\s+"[^"]{0,16}"\s+.{2}\s+(2[ac])\s*$'
)
DIRECTORY_ENTRY = re.compile(
    r'^\s*(\d+)\s+"([^"]*)"\s+(\*?)(del|seq|prg|usr|rel)(<?)\s*$'
)
DIRECTORY_EMPTY = "Empty image"
DIRECTORY_TRAILER = re.compile(r"^\s*\d+\s+blocks free\.\s*$")
EXTRACTED_ARCHIVE = re.compile(r"^Trying filename '([^']+)'$")
C1541_ERROR = re.compile(r"^(?:ERR\s*=|cannot\s|invalid filename$)", re.IGNORECASE)


@dataclass(frozen=True)
class Entry:
    blocks: int
    name: str
    file_type: str
    closed: bool = True
    locked: bool = False


def run_c1541(*arguments, cwd=None):
    """Run c1541 in batch mode and return its output."""
    command = ["c1541", *map(str, arguments)]
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except FileNotFoundError as error:
        raise RuntimeError("c1541 is not installed") from error
    except subprocess.CalledProcessError as error:
        output = error.stdout.rstrip() or "<no output>"
        raise RuntimeError(
            f"c1541 failed with exit status {error.returncode}:\n{output}"
        ) from error
    for line in result.stdout.splitlines():
        if C1541_ERROR.match(line):
            raise RuntimeError(f"c1541 reported an error:\n{result.stdout.rstrip()}")
    return result.stdout


def directory(path, expected_dos_type):
    """Parse one complete c1541 directory listing."""
    entries = []
    found_header = False
    found_trailer = False
    # Startup and attachment chatter appears outside these sentinels. Between
    # them, every line must be a directory entry so partial parsing is fatal.
    for line in run_c1541(path, "-list").splitlines():
        header = DIRECTORY_HEADER.fullmatch(line)
        if header:
            if found_header or found_trailer:
                raise ValueError(f"{path} has an unexpected directory header")
            if header.group(1) != expected_dos_type:
                raise ValueError(
                    f"{path} has DOS type {header.group(1)}, "
                    f"expected {expected_dos_type}"
                )
            found_header = True
            continue

        if DIRECTORY_TRAILER.fullmatch(line):
            if not found_header or found_trailer:
                raise ValueError(f"{path} has an unexpected directory trailer")
            found_trailer = True
            continue

        match = DIRECTORY_ENTRY.fullmatch(line)
        if found_header and not found_trailer:
            if line == DIRECTORY_EMPTY and not entries:
                continue
            if not match:
                raise ValueError(f"unexpected c1541 directory line for {path}: {line!r}")
            blocks, name, open_marker, file_type, lock_marker = match.groups()
            entries.append(
                Entry(int(blocks), name, file_type, not open_marker, bool(lock_marker))
            )
        elif match:
            raise ValueError(f"{path} has a directory entry outside its listing")

    if not found_header or not found_trailer:
        raise ValueError(f"c1541 returned an incomplete directory for {path}")
    return tuple(entries)


def source_entries(path):
    """Return supported metadata from a standard D64 image."""
    if not path.is_file():
        raise ValueError(f"{path} is not a regular file")
    if path.stat().st_size != D64_SIZE:
        raise ValueError(f"{path} is not a standard 35-track D64 image")
    entries = directory(path, "2a")
    if not entries:
        raise ValueError(f"{path} has no files")
    for entry in entries:
        if entry.file_type not in SUPPORTED_FILE_TYPES:
            raise ValueError(f"{path} contains an unsupported {entry.file_type} file")
        if not entry.closed:
            raise ValueError(f"{path} contains an open file")
        if entry.locked:
            raise ValueError(f"{path} contains a locked file")
        if not entry.blocks:
            raise ValueError(f"{path} contains an empty file")
    return entries


def extract(path, destination, entries):
    """Extract P00/S00 archives and return them in directory order."""
    output = run_c1541(path, "-p00save", "1", "-extract", cwd=destination)
    archives = []
    for line in output.splitlines():
        match = EXTRACTED_ARCHIVE.fullmatch(line)
        if match:
            archive = Path(match.group(1))
            if not archive.is_absolute():
                raise ValueError(f"c1541 returned a relative archive path: {archive}")
            archive = archive.resolve()
            if archive.parent != destination.resolve():
                raise ValueError(f"c1541 extracted {archive} outside its destination")
            archives.append(archive)

    # P00/S00 containers retain the raw PETSCII name and CBM file type.
    artifacts = tuple(destination.iterdir())
    if any(not item.is_file() for item in artifacts):
        raise ValueError(f"{path} produced a non-file extraction artifact")
    files = {item.resolve() for item in artifacts}
    if len(archives) != len(set(archives)):
        raise ValueError(f"{path} reported a duplicate c1541 archive")
    if len(archives) != len(entries) or set(archives) != files:
        raise ValueError(f"{path} produced unexpected c1541 archives")
    for entry, archive in zip(entries, archives, strict=True):
        expected_suffix = SUPPORTED_FILE_TYPES[entry.file_type]
        if archive.suffix.upper() != expected_suffix:
            raise ValueError(f"{path} produced an unexpected archive type")
    return tuple(archives)


def main():
    """Merge two supported D64 images into a c1541-formatted D80 image."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("sources", type=Path, nargs=2, metavar="SOURCE")
    parser.add_argument("--name", default="WATERLOO2")
    args = parser.parse_args()

    if C1541_VERSION not in run_c1541("-version"):
        raise RuntimeError(f"this script requires {C1541_VERSION}")
    if not DISK_NAME.fullmatch(args.name):
        raise ValueError("disk name must be 1-16 uppercase letters, digits, or spaces")

    output = args.output.resolve()
    sources = tuple(path.resolve() for path in args.sources)
    if output in sources:
        raise ValueError("output image must differ from both source images")
    if len(set(sources)) != len(sources):
        raise ValueError("source images must be distinct")

    # Inspect both sources before creating the output. Only the closed, unlocked
    # PRG and SEQ cases present on the Waterloo disks are accepted.
    source_metadata = tuple(source_entries(path) for path in sources)
    expected_entries = tuple(
        entry for entries in source_metadata for entry in entries
    )
    names = [entry.name.casefold() for entry in expected_entries]
    if len(names) != len(set(names)):
        raise ValueError("source images contain duplicate filenames")

    extracted = {}
    archives = []
    with tempfile.TemporaryDirectory(prefix="waterloo-d64-") as temporary:
        temporary_path = Path(temporary)

        # Let c1541 interpret each source and preserve its names and types in
        # P00/S00 containers. Keep the reported order for the destination.
        for index, (path, entries) in enumerate(
            zip(sources, source_metadata, strict=True)
        ):
            source_directory = temporary_path / str(index)
            source_directory.mkdir()
            source_archives = extract(path, source_directory, entries)
            for archive in source_archives:
                archive_name = archive.name.casefold()
                if archive_name in extracted:
                    raise ValueError("source images produce duplicate archive names")
                extracted[archive_name] = archive.read_bytes()
                archives.append(archive)

        # Let c1541 own D80 formatting, allocation, BAM updates, file chains,
        # and directory extension rather than reproducing CBM DOS internals.
        run_c1541("-format", f"{args.name},00", "d80", output)
        for archive in archives:
            run_c1541(output, "-p00save", "1", "-write", archive)

        if directory(output, "2c") != expected_entries:
            raise ValueError("merged D80 has unexpected c1541 directory metadata")

        # Re-extract the result and compare every container byte-for-byte. This
        # catches payload, PETSCII filename, type, and ordering differences.
        verification_directory = temporary_path / "verification"
        verification_directory.mkdir()
        verified_archives = extract(
            output,
            verification_directory,
            expected_entries,
        )
        verified = {
            archive.name.casefold(): archive.read_bytes()
            for archive in verified_archives
        }
        if verified.keys() != extracted.keys():
            raise ValueError("merged D80 contains unexpected c1541 archives")
        for name, contents in extracted.items():
            if verified[name] != contents:
                raise ValueError(f"merged D80 file {name} differs from its source")


if __name__ == "__main__":
    main()