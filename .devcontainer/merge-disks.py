#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Merge selected Commodore disk files into a D80 using VICE c1541.

import argparse
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


C1541_VERSION = "c1541 (VICE 3.9)"
SOURCE_FORMATS = {
    174848: ("D64", "2a"),
    533248: ("D80", "2c"),
}
SUPPORTED_FILE_TYPES = {"prg": ".P00", "seq": ".S00"}
C64FILE_MAGIC = b"C64File\0"
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
ARCHIVE_SUFFIX = re.compile(r"\.[PS]\d{2}$", re.IGNORECASE)
C1541_ERROR = re.compile(r"^(?:ERR\s*=|cannot\s|invalid filename$)", re.IGNORECASE)


@dataclass(frozen=True)
class Entry:
    blocks: int
    name: str
    file_type: str
    closed: bool = True
    locked: bool = False


@dataclass(frozen=True)
class Source:
    path: Path
    entries: tuple[Entry, ...]
    selected: tuple[Entry, ...]
    all_files: bool


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
    """Return complete metadata from a standard D64 or D80 image."""
    if not path.is_file():
        raise ValueError(f"{path} is not a regular file")
    try:
        _, dos_type = SOURCE_FORMATS[path.stat().st_size]
    except KeyError as error:
        raise ValueError(f"{path} is not a standard D64 or D80 image") from error
    entries = directory(path, dos_type)
    if not entries:
        raise ValueError(f"{path} has no files")
    names = [entry.name.casefold() for entry in entries]
    if len(names) != len(set(names)):
        raise ValueError(f"{path} contains duplicate filenames")
    return entries


def validate_entry(path, entry):
    """Reject entries which c1541 cannot preserve as P00 or S00."""
    if entry.file_type not in SUPPORTED_FILE_TYPES:
        raise ValueError(f"{path} file {entry.name} has unsupported {entry.file_type} type")
    if not entry.closed:
        raise ValueError(f"{path} file {entry.name} is open")
    if entry.locked:
        raise ValueError(f"{path} file {entry.name} is locked")
    if not entry.blocks:
        raise ValueError(f"{path} file {entry.name} is empty")


def select_entries(path, entries, selectors):
    """Select complete or named source entries in command-line order."""
    if not selectors:
        raise ValueError(f"{path} has no selectors")
    if "*" in selectors:
        if selectors != ("*",):
            raise ValueError(f"{path} must use '*' as its only all-files selector")
        selected = entries
    else:
        keys = [selector.casefold() for selector in selectors]
        if len(keys) != len(set(keys)):
            raise ValueError(f"{path} selects a filename more than once")
        by_name = {entry.name.casefold(): entry for entry in entries}
        selected = []
        for selector, key in zip(selectors, keys, strict=True):
            if key not in by_name:
                raise ValueError(f"{path} does not contain {selector}")
            selected.append(by_name[key])
        selected = tuple(selected)
    for entry in selected:
        validate_entry(path, entry)
    return tuple(selected)


def extract_archives(path, destination):
    """Extract all entries and return reported archives and created artifacts."""
    output = run_c1541(path, "-p00save", "1", "-extract", cwd=destination)
    archives = []
    resolved_destination = destination.resolve()
    for line in output.splitlines():
        match = EXTRACTED_ARCHIVE.fullmatch(line)
        if match:
            archive = Path(match.group(1))
            if not archive.is_absolute():
                raise ValueError(f"c1541 returned a relative archive path: {archive}")
            archive = archive.resolve()
            if archive.parent != resolved_destination:
                raise ValueError(f"c1541 extracted {archive} outside its destination")
            archives.append(archive)

    artifacts = tuple(destination.iterdir())
    if any(not item.is_file() for item in artifacts):
        raise ValueError(f"{path} produced a non-file extraction artifact")
    return tuple(archives), artifacts


def extract_supported(path, destination, entries):
    """Extract supported entries as P00/S00 archives in directory order."""
    archives, artifacts = extract_archives(path, destination)

    # P00/S00 containers retain the raw PETSCII name and CBM file type.
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


def archive_name(path):
    """Return the raw filename bytes stored in a C64File archive header."""
    header = path.read_bytes()[:24]
    if len(header) != 24 or not header.startswith(C64FILE_MAGIC):
        raise ValueError(f"{path} has an invalid C64File archive header")
    raw_name = header[len(C64FILE_MAGIC):]
    name_bytes, separator, padding = raw_name.partition(b"\0")
    if separator and padding.strip(b"\0"):
        raise ValueError(f"{path} has invalid C64File filename padding")
    return name_bytes


def extract_named(path, destination, entries):
    """Extract named ASCII P00/S00 archives in selected-entry order."""
    _, artifacts = extract_archives(path, destination)

    requested = {entry.name.casefold(): entry for entry in entries}
    archives = {}
    for artifact in artifacts:
        if not ARCHIVE_SUFFIX.fullmatch(artifact.suffix):
            continue
        try:
            name = archive_name(artifact).decode("ascii").casefold()
        except UnicodeDecodeError:
            continue
        if name not in requested:
            continue
        if name in archives:
            raise ValueError(f"{path} produced duplicate archive {name}")
        archives[name] = artifact
    if archives.keys() != requested.keys():
        missing = ", ".join(
            entry.name for entry in entries if entry.name.casefold() not in archives
        )
        raise ValueError(f"{path} did not extract requested archives: {missing}")
    return tuple(archives[entry.name.casefold()] for entry in entries)


def main():
    """Manufacture a c1541-formatted D80 from selected source files."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--name", default="WATERLOO2")
    parser.add_argument(
        "--source",
        action="append",
        nargs="+",
        required=True,
        metavar=("IMAGE", "SELECTOR"),
        help="source image followed by '*' or one or more filenames",
    )
    args = parser.parse_args()

    if C1541_VERSION not in run_c1541("-version"):
        raise RuntimeError(f"this script requires {C1541_VERSION}")
    if not DISK_NAME.fullmatch(args.name):
        raise ValueError("disk name must be 1-16 uppercase letters, digits, or spaces")

    output = args.output.resolve()
    specifications = tuple(
        (Path(group[0]).resolve(), tuple(group[1:])) for group in args.source
    )
    source_paths = tuple(path for path, _ in specifications)
    if output in source_paths:
        raise ValueError("output image must differ from all source images")
    if len(set(source_paths)) != len(source_paths):
        raise ValueError("source images must be distinct")

    sources = tuple(
        Source(
            path,
            entries,
            select_entries(path, entries, selectors),
            selectors == ("*",),
        )
        for path, selectors in specifications
        for entries in (source_entries(path),)
    )
    expected_entries = tuple(
        entry for source in sources for entry in source.selected
    )
    names = [entry.name.casefold() for entry in expected_entries]
    if len(names) != len(set(names)):
        raise ValueError("source images contain duplicate filenames")

    extracted = {}
    archives = []
    with tempfile.TemporaryDirectory(prefix="merge-disks-") as temporary:
        temporary_path = Path(temporary)

        # c1541's P00/S00 output preserves raw names and types. Wildcard
        # sources must extract cleanly in full, while named ASCII selections
        # identify their containers by the raw C64File archive header.
        for index, source in enumerate(sources):
            source_directory = temporary_path / str(index)
            source_directory.mkdir()
            if source.all_files:
                source_archives = extract_supported(
                    source.path,
                    source_directory,
                    source.selected,
                )
            else:
                source_archives = extract_named(
                    source.path,
                    source_directory,
                    source.selected,
                )
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
        verified_archives = extract_supported(
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