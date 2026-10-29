#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0

"""Shared VICE ROM-order, completion-monitor, and c1541 inspection helpers.

Use these alongside native reference captures without treating VICE's C FDC
as original controller-ROM execution or reconstructed transactions as wire
observations. Feature-specific capture/comparison scenarios are added with
their owning DOS features.
"""


from pathlib import Path
import re

import drive_reference as reference


READY = 0xb3ff
VARTAB = 0x2a
KEY_COUNT = 0x9e
KEY_BUFFER = 0x26f
TRACKS = {"4040": 35, "8050": 77, "8250": 154}
FILES = {"seed": "p", "one": "s", "bound254": "s", "bound255": "s", "m1check": "p"}


def compose_basic(roms):
    """Concatenate BASIC in CPU address order rather than MAME socket or part-number order."""
    return b"".join((roms / reference.CONTROLLERS[name]).read_bytes() for name in (
        "basic-4-b000.901465-23.bin", "basic-4-c000.901465-20.bin", "basic-4-d000.901465-21.bin"))


def write_monitors(output, model, program):
    """Load at BASIC READY, observe the real completion store and export unmodified active ROMs."""
    start, run, finish = (output / name for name in ("start.mon", "run.mon", "finish.mon"))
    end = reference.LOAD_ADDRESS + len(program.read_bytes()) - 2
    rom_start = 0xd000 if model == "4040" else 0xc000
    finish.write_text(
        f'bsave "{output / "capture.bin"}" 0 $4000 $5bff\n'
        f'bsave "{output / "mailbox.bin"}" 0 $7000 $701f\n'
        f'bsave "{output / "active-main.bin"}" 0 8:${rom_start:04x} 8:$ffff\n'
        f'bsave "{output / "active-basic.bin"}" 0 $b000 $dfff\n'
        f'bsave "{output / "active-kernal.bin"}" 0 $f000 $ffff\n'
        f'bsave "{output / "active-editor.bin"}" 0 $e000 $e7ff\n'
        "screen\nquit\n", encoding="ascii")
    run.write_text(
        f'delete\nload "{program}" 0\n'
        f'> ${VARTAB:04x} {end & 255:02x} {end >> 8:02x}\n'
        "watch store $7000\ncondition 1 if A == $ff\n"
        f'command 1 "playback \\"{finish}\\""\n'
        f'break ${READY:04x}\ncondition 2 if @ram:$7000 == $01\n'
        f'command 2 "playback \\"{finish}\\""\n'
        f'> ${KEY_BUFFER:04x} 52 55 4e 0d\n> ${KEY_COUNT:04x} 04\ng\n',
        encoding="ascii")
    start.write_text(
        f'break ${READY:04x}\ncommand 1 "playback \\"{run}\\""\ng\n', encoding="ascii")
    return start


def bam_observation(text, model):
    """Decode c1541's independently interpreted per-sector free/allocated display."""
    rows = {}
    for line in text.splitlines():
        match = re.fullmatch(r"\s*(\d+)\s+([.* ]+)", line)
        if match:
            track = int(match[1])
            marks = match[2].replace(" ", "")
            if track in rows or len(marks) != reference.image_sectors(model, track):
                raise ValueError("invalid or duplicate c1541 BAM row")
            rows[track] = marks
    if set(rows) != set(range(1, TRACKS[model] + 1)):
        raise ValueError("incomplete c1541 BAM coverage")
    return rows


def inspect_image(tool, model, image, directory, expected):
    """Check all c1541 BAM bits and preserve read-only directory/file observations."""
    directory.mkdir()
    reference.run([str(tool), "-attach", str(image), "-list"], directory / "list.log")
    reference.run([str(tool), "-attach", str(image), "-bam", "1", str(TRACKS[model])],
                  directory / "bam.log")
    listing = (directory / "list.log").read_text()
    bam = (directory / "bam.log").read_text()
    header = reference.image_offset(model, 18 if model == "4040" else 39, 0)
    label = header + (144 if model == "4040" else 6)
    name = expected[label:label + 16].rstrip(b"\xa0").decode("ascii").lower()
    disk_id = expected[label + 18:label + 20].decode("ascii").lower()
    directory_header = re.search(r'^0 "([^"]+)" (\S+) (2[ac])$', listing, re.MULTILINE)
    if directory_header is None or (
        directory_header[1].rstrip(), directory_header[2], directory_header[3]
    ) != (name, disk_id, "2a" if model == "4040" else "2c"):
        raise ValueError("c1541 directory label/ID/DOS type mismatch")
    rows = bam_observation(bam, model)
    for track, marks in rows.items():
        entry = reference.fixture_bam_entry(model, expected, track)
        wanted = "".join("." if expected[entry + 1 + sector // 8] & (1 << (sector % 8)) else "*"
                         for sector in range(reference.image_sectors(model, track)))
        if marks != wanted or marks.count(".") != expected[entry]:
            raise ValueError(f"c1541 BAM mismatch at track {track}")
    free = sum(marks.count(".") for track, marks in rows.items()
               if track != (18 if model == "4040" else 39))
    if f"{free} blocks free." not in listing:
        raise ValueError("c1541 directory free-block count mismatch")
    files = {}
    if "Empty image" not in listing:
        for name, file_type in FILES.items():
            destination = directory / (name + ".bin")
            reference.run([str(tool), "-attach", str(image), "-read", name + "," + file_type, str(destination)],
                          directory / (name + ".log"))
            files[name] = reference.sha256(destination)
    if image.read_bytes() != expected:
        raise ValueError("read-only c1541 inspection changed the image")
    return {"free_blocks": free, "files": files, "listing_sha256": reference.sha256(directory / "list.log"),
            "bam_sha256": reference.sha256(directory / "bam.log")}
