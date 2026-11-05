#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Rebuild pinned Commodore main-DOS sources and compare their emitted bytes.

Requires ACME and externally supplied sources, ROMs, and an identity manifest.
Usage: python3 tools/drive-roms/drive_rom_verify.py --help
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


SOURCE_COMMIT = "01bd60f162ef92212ef0cb67546ae8f42be34168"
ADDRESS_SPACE_SIZE = 0x10000
FAMILIES = {
    "4040": {
        "profile": "4040-dos20r2",
        "start": 0xD000,
        "digest": "76ab28fb08aff92a809b5976800456d693b8c8cd29beb04aa47c3d4722684eab",
        "checksums": {0xD2A0: ("romtbl", 10), 0xE7AE: ("echksm", 1), 0xFFE9: ("vector", 8)},
    },
    "8250": {
        "profile": "8050-dos27mpi",
        "start": 0xC000,
        "digest": "74196f9cf41e53ddbc768d74427b3f461da13067165eaeca6ab1a66821ffc8c8",
        "checksums": {0xC412: ("rom", 8), 0xE000: ("erom", 3)},
    },
}


def run_tool(command):
    """Capture successful tool output and forward failure diagnostics to case logs."""
    try:
        return subprocess.run(command, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout, end="", file=sys.stdout, flush=True)
        if error.stderr:
            print(error.stderr, end="", file=sys.stderr, flush=True)
        raise


def source_units(directory):
    """Return the complete, ordered source graph used by the pinned master."""
    master = (directory / "master").read_text(encoding="ascii")
    units = re.findall(r"^\.LIB (\w+)$", master, re.MULTILINE)
    if not units:
        raise ValueError("master has no recognized source units")
    return ["master"] + [unit.lower() for unit in units]


def source_digest(directory, units):
    """Identify raw input files, including source after each unit's END."""
    digest = hashlib.sha256()
    for unit in units:
        digest.update(unit.encode("ascii") + b"\0")
        digest.update(hashlib.sha256((directory / unit).read_bytes()).digest())
    return digest.hexdigest()


def translate_line(original):
    """Translate spelling only, preserving character literals and byte values."""
    code = re.sub(r"(#'.|'[^']*')|;.*", lambda match: match[1] or "", original)
    code = re.sub(r"#'(.)", lambda match: "#" + str(ord(match[1])), code)
    code = re.sub(
        r"'([^']*)'", lambda match: ",".join(str(ord(char)) for char in match[1]), code
    )
    code = re.sub(r"\.(BYTE|BYT)\b", "!byte", code)
    code = re.sub(r"\.WORD\b", "!word", code)
    code = re.sub(r"\)([XY])\b", r"),\1", code)
    code = re.sub(r"\b(ASL|LSR|ROL|ROR)\s+A\b", r"\1", code)
    return code.rstrip()


def translate(directory, units):
    """Expand units up to their first END and retain original line provenance."""
    output = ["!cpu 6502", "* = 0"]
    provenance = {}
    for unit in units:
        if unit == "master":
            continue
        for number, original in enumerate(
            (directory / unit).read_text(encoding="ascii").splitlines(), 1
        ):
            if re.match(r"\s*\.END\b", original):
                break
            if re.match(r"\s*\.(PAGE|PAG|SKIP|SKI|OPT)(?:\s|[0-9]|$)", original):
                continue
            code = translate_line(original)
            if not code.strip():
                continue
            if re.search(r"(^|\s)\.[A-Za-z]", code):
                raise ValueError(f"unsupported directive at {unit}:{number}: {code}")
            output.append(code)
            provenance[len(output)] = (unit, number, code)
    return "\n".join(output) + "\n", provenance


def emitted_addresses(report, provenance):
    """Read emitted coverage, including byte lists truncated in ACME reports."""
    emitted = {}
    for line in report.splitlines():
        match = re.match(r"\s*(\d+)\s+([0-9a-f]{4})\s+([0-9a-f]+)", line)
        if not match:
            continue
        line_number, address, data = match.groups()
        unit, number, code = provenance[int(line_number)]
        if "!byte" in code:
            length = len(code.split("!byte", 1)[1].split(","))
        elif "!word" in code:
            length = 2 * len(code.split("!word", 1)[1].split(","))
        else:
            length = len(data) // 2
        for offset in range(int(address, 16), int(address, 16) + length):
            if offset in emitted:
                raise ValueError(f"overlapping emitted byte at ${offset:04X}")
            if offset >= ADDRESS_SPACE_SIZE:
                raise ValueError("emitted bytes exceed the 6502 address space")
            emitted[offset] = (unit, number)
    if not emitted:
        raise ValueError("ACME report contains no recognized emitted bytes")
    return emitted


def address_ranges(addresses):
    """Represent exact coverage as inclusive ranges without filling gaps."""
    ranges = []
    for address in sorted(addresses):
        if ranges and address == ranges[-1][1] + 1:
            ranges[-1][1] = address
        else:
            ranges.append([address, address])
    return [[f"${start:04X}", f"${end:04X}"] for start, end in ranges]


def validate_coverage(image, alternate, emitted):
    """Cross-check listing coverage using two different fills for un-emitted gaps."""
    if len(image) < 3 or len(image) != len(alternate) or image[:2] != alternate[:2]:
        raise ValueError("fill-check images have inconsistent bounds")
    origin = int.from_bytes(image[:2], "little")
    observed = set()
    for index, (zero, filled) in enumerate(zip(image[2:], alternate[2:])):
        if zero == filled:
            observed.add(origin + index)
        elif (zero, filled) != (0, 255):
            raise ValueError("assembly changed beyond un-emitted fill bytes")
    if observed != emitted.keys():
        raise ValueError("listing coverage does not match independently checked emitted bytes")


def compare(built, origin, rom, start, emitted, checksums):
    """Compare only emitted bytes and explicitly identify checksum placeholders."""
    covered = {address: source for address, source in emitted.items() if address >= start}
    if not covered:
        raise ValueError("source emits no bytes inside the selected ROM")
    if origin < 0 or origin + len(built) > ADDRESS_SPACE_SIZE:
        raise ValueError("invalid rebuilt image bounds")
    if start + len(rom) != ADDRESS_SPACE_SIZE:
        raise ValueError("selected ROM must end at $FFFF")
    differences = []
    placeholders = []
    for address, (unit, line) in sorted(covered.items()):
        if not origin <= address < origin + len(built):
            raise ValueError(f"emitted byte outside rebuilt image at ${address:04X}")
        source_byte = built[address - origin]
        rom_byte = rom[address - start]
        row = {
            "address": f"${address:04X}", "unit": unit, "line": line,
            "source_byte": source_byte, "rom_byte": rom_byte,
        }
        if address in checksums:
            if (unit, line) != checksums[address] or source_byte != 0:
                raise ValueError(f"checksum placeholder changed at ${address:04X}")
            placeholders.append(row)
        elif source_byte != rom_byte:
            differences.append(row)
    if len(placeholders) != len(checksums):
        raise ValueError("not all expected checksum placeholders were emitted")
    return {
        "status": "mismatch" if differences else "emitted-bytes-match",
        "emitted_bytes": len(covered),
        "compared_bytes": len(covered) - len(placeholders),
        "matching_bytes": len(covered) - len(placeholders) - len(differences),
        "mismatch_count": len(differences),
        "checksum_placeholders": placeholders,
        "emitted_ranges": address_ranges(covered),
        "uncovered_ranges": address_ranges(set(range(start, ADDRESS_SPACE_SIZE)) - covered.keys()),
        "differences": differences,
        "limits": "not whole-ROM equality, drive execution, or controller-firmware verification",
    }


def load_rom(manifest, directory, family):
    """Validate approved main-ROM files before concatenating them in address order."""
    profile = next(
        item for item in manifest["drive_profiles"] if item["id"] == family["profile"]
    )
    parts = {item["part"]: item for item in manifest["drive_roms"]}
    result = bytearray()
    identities = []
    for part in profile["roms"]:
        item = parts[part]
        if not item["role"].startswith("main DOS"):
            continue
        data = (directory / (part + ".bin")).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if len(data) != item["size"] or digest != item["sha256"]:
            raise ValueError(f"ROM identity mismatch: {part}")
        result.extend(data)
        identities.append({"part": part, "sha256": digest, "size": len(data)})
    if family["start"] + len(result) != ADDRESS_SPACE_SIZE:
        raise ValueError("main-ROM manifest has an unexpected size or composition")
    return bytes(result), identities


def main():
    """Verify identity, rebuild locally, and return failure for any code mismatch."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", choices=FAMILIES, required=True)
    parser.add_argument("--source-dir", type=Path, required=True, help="DOS_4040 or DOS_8250 directory")
    parser.add_argument("--rom-dir", type=Path, required=True, help="externally acquired chip .bin files")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="private/ignored directory, never publish")
    args = parser.parse_args()
    family = FAMILIES[args.family]

    # Establish both input identities before generating any derived ROM material.
    manifest = json.loads(args.manifest.read_text(encoding="ascii"))
    if manifest["source"]["commit"] != SOURCE_COMMIT:
        raise ValueError("manifest does not identify the supported source commit")
    units = source_units(args.source_dir)
    digest = source_digest(args.source_dir, units)
    if digest != family["digest"]:
        raise ValueError("source files do not match the pinned source graph")
    rom, identities = load_rom(manifest, args.rom_dir, family)
    text, provenance = translate(args.source_dir, units)

    # Rebuild without altering upstream source files or patching mismatching bytes.
    args.output_dir.mkdir(parents=True, exist_ok=False)
    source = args.output_dir / "source.asm"
    binary = args.output_dir / "source.prg"
    alternate = args.output_dir / "source-fill.prg"
    report = args.output_dir / "source.report"
    source.write_text(text, encoding="ascii")
    subprocess.run(
        ["acme", "--strict", "--format", "cbm", "--initmem", "0", "--outfile", str(binary),
         "--report", str(report), "--symbollist", str(args.output_dir / "source.sym"),
         str(source)], check=True
    )
    data = binary.read_bytes()
    origin = int.from_bytes(data[:2], "little")
    emitted = emitted_addresses(report.read_text(encoding="ascii"), provenance)
    subprocess.run(
        ["acme", "--strict", "--format", "cbm", "--initmem", "255",
         "--outfile", str(alternate), str(source)], check=True
    )
    validate_coverage(data, alternate.read_bytes(), emitted)

    # Publish metadata separately from the private translated source and binary.
    result = compare(data[2:], origin, rom, family["start"], emitted, family["checksums"])
    result.update({
        "family": args.family, "source_commit": SOURCE_COMMIT, "source_digest": digest,
        "roms": identities,
        "assembler": run_tool(["acme", "--version"]).stdout.strip(),
    })
    (args.output_dir / "comparison.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="ascii"
    )
    print(f"{args.family}: {result['status']}, {result['matching_bytes']}/"
          f"{result['compared_bytes']} compared bytes match")
    return 1 if result["mismatch_count"] else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, StopIteration, subprocess.CalledProcessError) as error:
        print(f"drive ROM verification failed: {error}", file=sys.stderr)
        sys.exit(2)
