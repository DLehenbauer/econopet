#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
r"""Assemble source mappings and reconstruct payloads to check ROM
correspondence.

The manifest pins source digests, approved ROM hashes, and mapping recipes.
Input identities are checked before generating source or assembled binaries.

With --mapping, apply the declared source adaptations, assemble with ACME, and
compare emitted ROM bytes against the approved original binaries. Declared
checksum placeholders are excluded and uncovered ranges are reported. This
checks the adapted source's emitted bytes, not whole-ROM equality or equality of
the unmodified upstream source.

With --binary-map, disassemble a selected ROM payload, reassemble it with
ca65/ld65, and require byte-for-byte equality across the entire selected
payload.

Retain generated source, assembled binaries, and reports in --output-dir.
Neither mode verifies drive execution, behavioral compatibility, or timing.

Examples (from the repository root, after tools/drive-roms/verify.sh acquires
the inputs). Each output directory must be new.

Assemble the adapted 4040 revision-2 source and compare emitted ROM bytes:

    python3 tools/drive-roms/drive_rom_map.py \
        --mapping 4040-revision2 \
        --source-dir .tmp/ai/cbmsrc/DOS_4040 \
        --rom-dir .tmp/ai/drive-roms/download \
        --manifest tools/drive-roms/drive-emulation-roms.json \
        --output-dir .tmp/ai/drive-roms/out/manual-4040-revision2

Disassemble and reassemble the 4040 controller payload with exact equality:

    python3 tools/drive-roms/drive_rom_map.py \
        --binary-map 4040-controller \
        --rom-dir .tmp/ai/drive-roms/download \
        --manifest tools/drive-roms/drive-emulation-roms.json \
        --output-dir .tmp/ai/drive-roms/out/manual-4040-controller
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import drive_rom_verify as verify


def composition(directory, master):
    """Read one ordered, non-recursive library composition without path escapes."""
    if not re.fullmatch(r"[A-Za-z0-9_]+", master):
        raise ValueError("invalid composition filename")
    text = (directory / master).read_text(encoding="utf-8")
    text = re.split(r"(?m)^\s*\.END\b", text, maxsplit=1)[0]
    units = re.findall(r"^\s*\.LIB\s+([A-Za-z0-9_]+)\s*$", text, re.MULTILINE)
    if (not units or master.lower() in {unit.lower() for unit in units}
            or len(set(unit.lower() for unit in units)) != len(units)):
        raise ValueError("empty or repeated source units")
    return [master] + [unit.lower() for unit in units]


def translate_mapping(directory, units, patches):
    """Apply declared line replacements before spelling translation, never bytes."""
    spans = {}
    for patch in patches:
        unit, first, last = patch["unit"], patch["first"], patch["last"]
        if (unit not in units[1:] or type(first) is not int or type(last) is not int
                or not 1 <= first <= last
                or not isinstance(patch["replacement"], list)
                or any(not isinstance(line, str) for line in patch["replacement"])):
            raise ValueError("invalid patch range")
        for number in range(first, last + 1):
            key = (unit, number)
            if key in spans:
                raise ValueError("overlapping patch ranges")
            spans[key] = patch

    output = ["!cpu 6502", "* = 0"]
    provenance = {}
    visited = set()
    for unit in units[1:]:
        for number, original in enumerate(
            (directory / unit).read_text(encoding="utf-8").splitlines(), 1
        ):
            if re.match(r"\s*\.END\b", original):
                break
            key = (unit, number)
            patch = spans.get(key)
            visited.add(key)
            if patch is not None:
                lines = patch["replacement"] if number == patch["first"] else []
            else:
                lines = [original]
            for line in lines:
                if not line.isascii():
                    raise ValueError(f"non-ASCII source at {unit}:{number}")
                if re.match(r"\s*\.(PAGE|PAG|SKIP|SKI|OPT)(?:\s|[0-9]|$)", line):
                    continue
                code = verify.translate_line(line)
                if not code.strip():
                    continue
                if not code.isascii() or re.search(r"(^|\s)\.[A-Za-z]", code):
                    raise ValueError(f"unsupported source at {unit}:{number}")
                output.append(code)
                provenance[len(output)] = (unit, number, code)
    if spans.keys() - visited:
        raise ValueError("patch includes missing or post-END source lines")
    return "\n".join(output) + "\n", provenance


def assemble(directory, text, provenance):
    """Cross-check emitted coverage with independently initialized ACME images."""
    directory.mkdir()
    source = directory / "source.asm"
    source.write_text(text, encoding="ascii")
    binary = directory / "source.prg"
    report = directory / "source.report"
    verify.run_tool(
        ["acme", "--strict", "--format", "cbm", "--initmem", "0",
         "--outfile", str(binary), "--report", str(report),
         "--symbollist", str(directory / "source.sym"), str(source)]
    )
    alternate = directory / "source-fill.prg"
    verify.run_tool(
        ["acme", "--strict", "--format", "cbm", "--initmem", "255",
         "--outfile", str(alternate), str(source)]
    )
    data = binary.read_bytes()
    emitted = verify.emitted_addresses(report.read_text(encoding="ascii"), provenance)
    verify.validate_coverage(data, alternate.read_bytes(), emitted)
    return int.from_bytes(data[:2], "little"), data[2:], emitted


def approved_rom(manifest, directory, recipe):
    """Load only selected-profile parts in the declared contiguous address order."""
    profiles = {item["id"]: item for item in manifest["drive_profiles"]}
    profile = profiles[recipe["profile"]]
    parts = {item["part"]: item for item in manifest["drive_roms"]}
    if (not recipe["parts"] or type(recipe["start"]) is not int
            or not 0 <= recipe["start"] < verify.ADDRESS_SPACE_SIZE):
        raise ValueError("invalid ROM composition bounds")
    rom = bytearray()
    identities = []
    for part in recipe["parts"]:
        if part not in profile["roms"] or part in identities:
            raise ValueError("mapping includes an unapproved or repeated ROM part")
        item = parts[part]
        data = (directory / (part + ".bin")).read_bytes()
        if len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError(f"ROM identity mismatch: {part}")
        rom.extend(data)
        identities.append(part)
    if recipe["start"] + len(rom) != verify.ADDRESS_SPACE_SIZE:
        raise ValueError("mapped ROM image does not end at $FFFF")
    return bytes(rom), identities


def compare_mapping(origin, data, emitted, rom, recipe):
    """Account for every emitted byte, declared checksum, and uncovered interval."""
    start = recipe["start"]
    if not 0 <= start < verify.ADDRESS_SPACE_SIZE or start + len(rom) != verify.ADDRESS_SPACE_SIZE:
        raise ValueError("invalid selected ROM bounds")
    if origin < 0 or origin + len(data) > verify.ADDRESS_SPACE_SIZE:
        raise ValueError("invalid assembly bounds")
    covered = {address: source for address, source in emitted.items() if address >= start}
    if not covered:
        raise ValueError("mapping emits no ROM bytes")
    checksums = {item["address"]: item for item in recipe["checksums"]}
    if len(checksums) != len(recipe["checksums"]):
        raise ValueError("repeated checksum exclusion")
    if checksums.keys() - covered.keys():
        raise ValueError("checksum exclusion outside emitted coverage")
    mismatches = []
    exclusions = []
    for address, (unit, line) in sorted(covered.items()):
        if not origin <= address < origin + len(data):
            raise ValueError("emitted byte outside assembly")
        source_byte = data[address - origin]
        target_byte = rom[address - start]
        exclusion = checksums.get(address)
        if exclusion is not None:
            if (unit, line, source_byte) != (
                exclusion["unit"], exclusion["line"], exclusion["source_value"]
            ):
                raise ValueError("checksum exclusion source identity changed")
            exclusions.append({"address": address, "source_value": source_byte,
                               "rom_value": target_byte})
        elif source_byte != target_byte:
            mismatches.append({"address": address, "unit": unit, "line": line,
                               "source_value": source_byte, "rom_value": target_byte})
    holes = set(range(start, start + len(rom))) - covered.keys()
    return {
        "emitted_bytes": len(covered),
        "compared_bytes": len(covered) - len(exclusions),
        "matching_bytes": len(covered) - len(exclusions) - len(mismatches),
        "mismatch_count": len(mismatches),
        "checksum_exclusions": exclusions,
        "emitted_ranges": verify.address_ranges(covered),
        "uncovered_ranges": verify.address_ranges(holes),
        "uncovered_values": sorted({rom[address - start] for address in holes}),
        "differences": mismatches,
    }


def source_map(original, mapped):
    """Retain exact old/new coverage for each source line, including deletions."""
    rows = {}
    for side, coverage in (("original", original), ("mapped", mapped)):
        for address, (unit, line) in coverage.items():
            row = rows.setdefault((unit, line), {"original": [], "mapped": []})
            row[side].append(address)
    return [
        {"unit": unit, "line": line,
         "original": verify.address_ranges(row["original"]),
         "mapped": verify.address_ranges(row["mapped"])}
        for (unit, line), row in sorted(rows.items())
    ]


def disassemble(directory, rom, recipe):
    """Round-trip a pinned payload with explicit data ranges and entry labels."""
    offset, size, origin = recipe["offset"], recipe["size"], recipe["runtime"]
    if (any(type(value) is not int for value in (offset, size, origin))
            or not 0 <= offset < offset + size <= len(rom)
            or not 0 <= origin < origin + size <= verify.ADDRESS_SPACE_SIZE):
        raise ValueError("invalid payload bounds")
    data = rom[offset:offset + size]
    if hashlib.sha256(data).hexdigest() != recipe["payload_sha256"]:
        raise ValueError("payload identity mismatch")
    info = [f'GLOBAL {{ STARTADDR ${origin:04X}; CPU "6502"; }};']
    declared_data = set()
    for first, last, kind in recipe["data_ranges"]:
        if not origin <= first <= last < origin + size or kind not in ("byte", "word"):
            raise ValueError("invalid disassembly data range")
        addresses = set(range(first, last + 1))
        if declared_data & addresses or (kind == "word" and len(addresses) % 2):
            raise ValueError("overlapping or incomplete disassembly data range")
        declared_data.update(addresses)
        table = "ByteTable" if kind == "byte" else "WordTable"
        info.append(f"RANGE {{ START ${first:04X}; END ${last:04X}; TYPE {table}; }};")
    for name, address in recipe["entries"].items():
        if (not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name)
                or not origin <= address < origin + size or address in declared_data):
            raise ValueError("invalid disassembly entry")
        info.append(f'LABEL {{ ADDR ${address:04X}; NAME "{name}"; }};')

    # Keep ROM-derived disassembly and object files in a fresh private directory.
    directory.mkdir(parents=True, exist_ok=False)
    payload = directory / "payload.bin"
    payload.write_bytes(data)
    configuration = directory / "source.info"
    configuration.write_text("\n".join(info) + "\n", encoding="ascii")
    source = directory / "source.asm"
    verify.run_tool(
        ["da65", "--info", str(configuration), "--comments", "4",
         "-o", str(source), str(payload)]
    )

    # Exact reconstruction checks bytes, not reachability or behavioral correctness.
    obj = directory / "source.o"
    rebuilt = directory / "rebuilt.bin"
    verify.run_tool(
        ["ca65", "-o", str(obj), str(source)]
    )
    verify.run_tool(
        ["ld65", "-t", "none", "-S", str(origin), "-o", str(rebuilt), str(obj)]
    )
    if rebuilt.read_bytes() != data:
        raise ValueError("disassembly reconstruction differs from selected payload")
    return {
        "payload_bytes": len(data), "payload_sha256": recipe["payload_sha256"],
        "runtime_ranges": verify.address_ranges(range(origin, origin + len(data))),
        "data_ranges": recipe["data_ranges"], "entries": recipe["entries"],
        "roundtrip": "all payload bytes match",
        "limits": "classified static disassembly, not source equality or drive execution",
    }


def main():
    """Verify both input identities before generating a fresh private mapping."""
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--mapping", help="manifest source_mappings key")
    selection.add_argument("--binary-map", help="manifest binary_mappings key")
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--rom-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="ascii"))
    if manifest["source"]["commit"] != verify.SOURCE_COMMIT:
        raise ValueError("unexpected source commit")
    if args.binary_map:
        recipe = manifest["binary_mappings"][args.binary_map]
        rom, parts = approved_rom(manifest, args.rom_dir, recipe)
        result = disassemble(args.output_dir, rom, recipe)
        result.update({
            "mapping": args.binary_map, "rom_parts": parts,
            "recipe_digest": hashlib.sha256(
                json.dumps(recipe, sort_keys=True).encode("ascii")
            ).hexdigest(),
            "disassembler": verify.run_tool(["da65", "--version"]).stderr.strip(),
        })
        (args.output_dir / "mapping.json").write_text(
            json.dumps(result, indent=2) + "\n", encoding="ascii"
        )
        print(f"{args.binary_map}: {result['payload_bytes']} bytes reconstruct exactly")
        return 0
    if args.source_dir is None:
        parser.error("--source-dir is required for --mapping")
    recipe = manifest["source_mappings"][args.mapping]
    units = composition(args.source_dir, recipe["master"])
    digest = verify.source_digest(args.source_dir, units)
    if digest != recipe["source_digest"]:
        raise ValueError("source files do not match the pinned source graph")
    rom, parts = approved_rom(manifest, args.rom_dir, recipe)
    original_text, original_provenance = translate_mapping(args.source_dir, units, [])
    mapped_text, mapped_provenance = translate_mapping(
        args.source_dir, units, recipe["patches"]
    )

    # Keep both the unmodified source baseline and explicit adaptation observable.
    args.output_dir.mkdir(parents=True, exist_ok=False)
    _, _, original_coverage = assemble(
        args.output_dir / "original", original_text, original_provenance
    )
    origin, data, mapped_coverage = assemble(
        args.output_dir / "mapped", mapped_text, mapped_provenance
    )
    result = compare_mapping(origin, data, mapped_coverage, rom, recipe)
    result.update({
        "mapping": args.mapping, "source_commit": verify.SOURCE_COMMIT,
        "source_digest": digest, "rom_parts": parts,
        "recipe_digest": hashlib.sha256(
            json.dumps(recipe, sort_keys=True).encode("ascii")
        ).hexdigest(),
        "patches": recipe["patches"],
        "source_map": source_map(original_coverage, mapped_coverage),
        "assembler": verify.run_tool(["acme", "--version"]).stdout.strip(),
        "limits": "explicit source adaptation, not unmodified-source equality or drive execution",
    })
    (args.output_dir / "mapping.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="ascii"
    )
    print(f"{args.mapping}: {result['matching_bytes']}/{result['compared_bytes']} "
          f"compared bytes match, {result['mismatch_count']} mismatches")
    return 1 if result["mismatch_count"] else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"drive ROM mapping failed: {error}", file=sys.stderr)
        sys.exit(2)
