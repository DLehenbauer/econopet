#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Corroborate full NEW/DUPLICATE with approved main-DOS ROMs and VICE's C FDC."""

import argparse
import csv
import json
from pathlib import Path
import re
import subprocess

import drive_reference as reference
from drive_wire import decode_wire


READY = 0xb3ff
VARTAB = 0x2a
KEY_COUNT = 0x9e
KEY_BUFFER = 0x26f
CYCLE_LIMIT = 900_000_000
WALL_LIMIT = 180
REPLAY_WALL_LIMIT = 900
TRACKS = {"4040": 35, "8050": 77, "8250": 154}
FILES = {"seed": "p", "one": "s", "bound254": "s", "bound255": "s", "m1check": "p"}


def compose_basic(roms):
    """Concatenate BASIC in CPU address order rather than MAME socket or part-number order."""
    return b"".join((roms / reference.CONTROLLERS[name]).read_bytes() for name in (
        "basic-4-b000.901465-23.bin", "basic-4-c000.901465-20.bin", "basic-4-d000.901465-21.bin"))


def fixture_arguments(args):
    """Supply the shared native fixture generator without changing its scenario semantics."""
    values = dict(model=args.model, format_check=args.variant, c1541=args.c1541, petcat=args.petcat)
    for name in (
        "board_check", "ordinary_check", "write_version_check", "block_map_check",
        "block_write_check", "block_read_check", "reader_pointer_check", "validate_check",
        "copy_all_check", "user_interrupt_check", "warm_reset_check", "pointer_check",
        "direct_check", "copy_check", "scratch_check", "rename_check", "command_check",
    ):
        values[name] = False
    return argparse.Namespace(**values)


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


def capture(args):
    """Run a bounded disposable reference, requiring every observation and complete raw image."""
    output = args.output.resolve()
    reference.check_output_directory(output)
    manifest = json.loads(args.manifest.read_text())
    identities = reference.prepare_roms(args, output, manifest)
    initial, final, program = reference.prepare_image(fixture_arguments(args), output)
    roms = output / "roms" / "cbm4032"
    # VICE maps this file in address order, not MAME socket-number order.
    basic = compose_basic(roms)
    (output / "private-basic.bin").write_bytes(basic)
    parts = reference.MODELS[args.model][1]
    main = b"".join((output / "roms" / ("c" + args.model) / (part + "." + reference.SUFFIXES[part])).read_bytes()
                    for part in parts)
    (output / "private-main.bin").write_bytes(main)
    monitor = write_monitors(output, args.model, program)
    command = [
        str(args.vice), "-default", "-console", "-warp", "+sound", "-model", "4032", "+crtc",
        "-limitcycles", str(CYCLE_LIMIT), "-jamaction", "5", "-initbreak", "reset",
        "-kernal", str(roms / reference.CONTROLLERS["kernal-4.901465-22.bin"]),
        "-editor", str(roms / reference.CONTROLLERS["edit-4-n.901447-29.bin"]),
        "-chargen", str(roms / reference.CONTROLLERS["characters-2.901447-10.bin"]),
        "-basic", str(output / "private-basic.bin"),
        "-dos4040" if args.model == "4040" else "-dos1001", str(output / "private-main.bin"),
        "-drive8type", args.model, "-drive8truedrive", "-drive8idle", "0", "+virtualdev8",
        "-8", str(final), "-moncommands", str(monitor),
        "-monlog", "-monlogname", str(output / "monitor.txt"),
    ]
    extension = reference.MODELS[args.model][0]
    source = output / f"source-initial.{extension}"
    if args.variant == "duplicate":
        command.extend(("-8d1", str(output / f"source-final.{extension}")))
    (output / "invocation.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / "vice.log").open("wb") as log:
        subprocess.run(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=WALL_LIMIT)
    if (output / "active-main.bin").read_bytes() != main:
        raise ValueError("VICE active main-DOS ROM differs from approved bytes")
    if (output / "active-basic.bin").read_bytes() != basic:
        raise ValueError("VICE BASIC ROM address order differs")
    for name, staged in (
        ("kernal", "kernal-4.901465-22.bin"), ("editor", "edit-4-n.901447-29.bin"),
    ):
        if (output / f"active-{name}.bin").read_bytes() != (roms / reference.CONTROLLERS[staged]).read_bytes():
            raise ValueError(f"VICE active {name} ROM differs")
    observed = reference.decode_capture((output / "capture.bin").read_bytes(),
                                        (output / "mailbox.bin").read_bytes(),
                                        reference.format_layout(args.variant))
    reference.check_block_observations(args.model, observed, reference.format_steps(args.model, args.variant),
                                       reference.format_layout(args.variant), "VICE format")
    reference.check_format_media(args.model, args.variant, initial, final, source)
    if args.variant == "duplicate":
        reference.check_copy_all_source(source, output / f"source-final.{extension}")
    result = {
        "kind": "corroborative-main-dos-rom-execution", "model": args.model, "variant": args.variant,
        "emulator_sha256": reference.sha256(args.vice), "command": command, "staged_rom_inputs": identities,
        "harness_sha256": reference.sha256(Path(__file__)),
        "support_sha256": {name: reference.sha256(Path(__file__).with_name(name))
                           for name in ("drive_reference.py", "drive_wire.py")},
        "manifest_sha256": reference.sha256(args.manifest),
        "petcat_sha256": reference.sha256(args.petcat),
        "main_rom_sha256": reference.sha256(output / "active-main.bin"),
        "program_sha256": reference.sha256(program), "initial_sha256": reference.sha256(initial),
        "final_sha256": reference.sha256(final), "observations": observed,
        "capture_sha256": reference.sha256(output / "capture.bin"),
        "mailbox_sha256": reference.sha256(output / "mailbox.bin"),
        "limits": ["C-emulated FDC jobs, not original controller-ROM execution",
                   "no observed IEEE wire trace", "not physical timing, fault or board acceptance"],
    }
    if args.variant == "duplicate":
        result["source_initial_sha256"] = reference.sha256(source)
        result["source_final_sha256"] = reference.sha256(output / f"source-final.{extension}")
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="ascii")
    if args.replay_tool:
        compare(args, output, observed, initial, final, source)
    print(f"{args.model} {args.variant}: complete main-ROM observations and raw image verified")


def protocol_actions(model, variant, observed):
    """Reconstruct documented KERNAL transactions, never represent them as observed VICE wire."""
    reference.check_block_observations(model, observed, reference.format_steps(model, variant),
                                       reference.format_layout(variant), "VICE protocol")
    actions = []

    def host(case, byte, atn=1, eoi=0):
        """Append one accepted host byte with explicit command/data and EOI flags."""
        actions.append((case, "H", byte, atn, eoi))

    def release():
        """End the KERNAL's ATN phase separately from byte acceptance."""
        actions.append(("control", "A", 0, 0, 0))

    def listen(case, secondary, payload=b""):
        """Send PRINT, OPEN or CLOSE using the same proven KERNAL bus primitives."""
        host(case, 0x28)
        host(case, secondary)
        release()
        for index, value in enumerate(payload):
            host(case, value, 0, int(index + 1 == len(payload)))
        host(case, 0x3f)
        release()

    data_secondary = 0
    for name, _, channel, _, _, _ in reference.format_steps(model, variant):
        if name == "format_bad_syntax":
            listen(name, 0x6f, b"D0=0\r" if variant == "duplicate" else b"NOCOLON\r")
        elif name == "format_success":
            text = b"D0=1\r" if variant == "duplicate" else b"N0:M4 FORMATTED" + (
                b",AB\r" if variant == "full" else b"\r")
            listen(name, 0x6f, text)
        elif name in ("format_listing", "format_reopened"):
            data_secondary = 0
            listen(name, 0xf0, b"$0")
        elif name == "format_initialize":
            listen(name, 0xe0)
            listen(name, 0x6f, b"I0\r")
        elif name == "format_copied_file":
            listen(name, 0xe0)
            data_secondary = 2
            listen(name, 0xf2, b"0:SEED,P,R")
        secondary = 15 if channel == 15 else data_secondary
        sample = observed[name]
        for value, status in zip(sample["bytes"], sample["kernal_st"]):
            if status not in (0, 64):
                raise ValueError("unreplayable VICE timeout/error observation")
            host(name, 0x48)
            host(name, 0x60 + secondary)
            release()
            actions.append((name, "R", value, 0, int(status == 64)))
            host(name, 0x5f)
            release()
    listen(name, 0xe0 + data_secondary)
    listen(name, 0xef)
    return actions


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


def compare(args, output, observed, initial, final, source):
    """Require full production action/observation/image coverage and independent c1541 inspection."""
    actions = protocol_actions(args.model, args.variant, observed)
    script = output / "production-replay.txt"
    script.write_text("ieee-replay-v1\n" + "".join(
        " ".join(map(str, action)) + "\n" for action in actions), encoding="ascii")
    candidate = output / "candidate"
    invocation = [str(args.replay_tool.resolve()), str(initial), str(script), str(candidate)]
    if args.variant == "duplicate":
        invocation.append(str(source))
    with (output / "replay.log").open("wb") as log:
        subprocess.run(invocation, stdout=log, stderr=subprocess.STDOUT, check=True,
                       timeout=REPLAY_WALL_LIMIT)
    coverage = dict(line.split("=", 1) for line in (candidate / "coverage.txt").read_text().splitlines())
    if int(coverage["completed"]) != len(actions) or int(coverage["requested"]) != len(actions) or coverage["failure"]:
        raise ValueError("incomplete production replay")
    with (candidate / "received.csv").open() as stream:
        received = list(csv.DictReader(stream))
    reads = [(index, action) for index, action in enumerate(actions) if action[1] == "R"]
    if len(received) != len(reads):
        raise ValueError("production observation count mismatch")
    for actual, (index, action) in zip(received, reads):
        if (actual["case"], int(actual["action"]), int(actual["actual"]), int(actual["actual_eoi"])) != (
                action[0], index, action[2], action[4]):
            raise ValueError(f"production observation mismatch: {actual}")
    wire = decode_wire(candidate / "ieee-wire.csv")
    transfers = [action for action in actions if action[1] != "A"]
    if not wire["coverage"]["complete"] or wire["issues"] or wire["aborted"] or len(wire["frames"]) != len(transfers):
        raise ValueError("incomplete or invalid production bus trace")
    for frame, action in zip(wire["frames"], transfers):
        if (frame["byte"], frame["atn"], frame["eoi"], frame["direction"]) != (
                action[2], bool(action[3]), bool(action[4]), "host" if action[1] == "H" else "drive"):
            raise ValueError("production bus differs from reconstructed stimulus")
    extension = reference.MODELS[args.model][0]
    persisted = candidate / f"final.{extension}"
    reference.check_format_media(args.model, args.variant, initial, persisted, source)
    if final.read_bytes() != persisted.read_bytes():
        raise ValueError("VICE/production complete image mismatch")
    inspections = {}
    expected = final.read_bytes()
    for name, path in (("vice", final), ("production", persisted)):
        inspections[name] = inspect_image(args.c1541, args.model, path, output / ("c1541-" + name), expected)
    if args.variant == "duplicate":
        reference.check_copy_all_source(source, candidate / f"source-final.{extension}")
        source_final = output / f"source-final.{extension}"
        reference.check_copy_all_source(source, source_final)
        inspections["source"] = inspect_image(
            args.c1541, args.model, source, output / "c1541-source", source.read_bytes())
        if any(set(value["files"]) != set(FILES)
               or value["files"] != inspections["source"]["files"] for value in inspections.values()):
            raise ValueError("c1541 duplicate file content differs")
    elif any(value["files"] for value in inspections.values()):
        raise ValueError("c1541 full NEW directory is not empty")
    report = {
        "kind": "VICE-main-ROM-versus-production-corroboration", "observations": len(observed),
        "actions": len(actions), "received_bytes": len(received), "frames": len(transfers),
        "initial_sha256": reference.sha256(initial), "final_sha256": reference.sha256(final),
        "replay_tool_sha256": reference.sha256(args.replay_tool),
        "replay_sha256": reference.sha256(script), "reference_sha256": reference.sha256(output / "result.json"),
        "candidate_sha256": reference.sha256(persisted),
        "received_sha256": reference.sha256(candidate / "received.csv"),
        "coverage_sha256": reference.sha256(candidate / "coverage.txt"),
        "wire_sha256": reference.sha256(candidate / "ieee-wire.csv"),
        "c1541_sha256": reference.sha256(args.c1541), "inspections": inspections,
        "limitations": ["host transactions reconstructed from documented KERNAL protocol",
                        "not an observed VICE IEEE wire/timing comparison",
                        "VICE C FDC is not original controller-ROM execution"],
    }
    if args.variant == "duplicate":
        report["source_sha256"] = reference.sha256(source)
        report["candidate_source_sha256"] = reference.sha256(candidate / f"source-final.{extension}")
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")


def main():
    """Accept explicit approved inputs and require a fresh ignored evidence directory."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", choices=reference.MODELS, required=True)
    parser.add_argument("--variant", choices=("full", "duplicate"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=Path("tools/drive-roms/drive-emulation-roms.json"))
    parser.add_argument("--drive-rom-dir", type=Path, default=Path(".cache/ai/m3-native-inputs"))
    parser.add_argument("--controller-rom-dir", type=Path, default=Path(".cache/ai/m3-native-inputs"))
    parser.add_argument("--gcr-rom", type=Path, default=Path(".cache/ai/m3-native-inputs/901467.bin"))
    parser.add_argument("--vice", type=Path, default=Path("/usr/bin/xpet"))
    parser.add_argument("--c1541", type=Path, default=Path("/usr/bin/c1541"))
    parser.add_argument("--petcat", type=Path, default=Path("/usr/bin/petcat"))
    parser.add_argument("--replay-tool", type=Path)
    capture(parser.parse_args())


if __name__ == "__main__":
    main()
