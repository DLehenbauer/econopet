#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Run pinned full-drive ROM cases on disposable D64/D80/D82 images.

Requires external approved ROMs, the separate 901467 GCR hardware lookup ROM,
MAME 0.285 and VICE's c1541/petcat fixture tools. A pinned,
single-cycle-interleaved source build adds composed-net IEEE traces.
"""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from drive_wire import attoseconds, check_wire_bytes, decode_wire, label_cases, write_replay


MAME_COMMIT = "f379cd060ac3b2bd567ebbd0957aed059d6895ff"
MAME_SHA256 = "b8de871c99effd28e90c426b811694811bcb533741189f93ad63a962621ae40c"
GCR_SHA1 = "97df576397608455616331f8e837cb3404363fa2"
LOAD_ADDRESS = 0x0401
CAPTURE_ADDRESS = 0x4000
MAILBOX_ADDRESS = 0x7000
PHASE_ADDRESS = 0x701f
BASIC_MEMSIZ_LOW = 0x34
BASIC_MEMSIZ_HIGH = 0x35
COMPLETE = 255
PET_VIA_PORT_B = 0xe840
PET_PIA2_PORT_B = 0xe822
PET_ATN_MASK = 0x04
BUS_RELEASED = 0xff
TALK_DEVICE_8 = 0x48
DATA_SECONDARY = 0x62
STATUS_SECONDARY = 0x6f
SEED = bytes((0, 0x20)) + bytes(range(256)) + b"END"
WRITTEN = b"M1 ROM WRITE\r"

MODELS = {
    "4040": ("d64", ("901468-14", "901468-15", "901468-16"), "901466-04"),
    "8050": ("d80", ("901887-01", "901888-01"), "901869-01"),
    "8250": ("d82", ("901887-01", "901888-01"), "901869-01"),
}

SUFFIXES = {
    "901468-14": "uj1", "901468-15": "ul1", "901468-16": "uh1",
    "901466-04": "uk3", "901887-01": "ul1", "901888-01": "uh1",
    "901869-01": "uk3",
}

CONTROLLERS = {
    "basic-4-b000.901465-23.bin": "901465-23.ud5",
    "basic-4-c000.901465-20.bin": "901465-20.ud6",
    "basic-4-d000.901465-21.bin": "901465-21.ud7",
    "edit-4-n.901447-29.bin": "901447-29.ud8",
    "kernal-4.901465-22.bin": "901465-22.ud9",
    "characters-2.901447-10.bin": "901447-10.uf10",
}

LAYOUT = {
    "startup": (0x4000, 0x4040, 1, 64),
    "consumed": (0x4080, 0x40c0, 2, 64),
    "seed": (0x4200, 0x4400, 3, 512),
    "interposed": (0x4600, 0x4640, 5, 64),
    "boundary254": (0x4800, 0x4a00, 8, 512),
    "boundary255": (0x4c00, 0x4e00, 10, 512),
    "readback": (0x5000, 0x5200, 12, 512),
    "onebyte": (0x5400, 0x5600, 14, 512),
    "written_status": (0x5800, 0x5840, 16, 64),
    "reset_status": (0x5880, 0x58c0, 17, 64),
}


def image_sectors(model, track):
    """Return native geometry independently of the firmware image layer."""
    if not 1 <= track <= (35 if model == "4040" else 77 if model == "8050" else 154):
        raise ValueError("invalid fixture track")
    if model == "4040":
        return 21 if track <= 17 else 19 if track <= 24 else 18 if track <= 30 else 17
    track = (track - 1) % 77 + 1
    return 29 if track <= 39 else 27 if track <= 53 else 25 if track <= 64 else 23


def image_offset(model, track, sector):
    """Locate one raw sector without consulting firmware or emulator metadata."""
    if not 0 <= sector < image_sectors(model, track):
        raise ValueError("invalid fixture sector")
    return (sum(image_sectors(model, previous) for previous in range(1, track)) + sector) * 256


def sha256(path):
    """Identify an exact input or persisted artifact."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, log, **kwargs):
    """Execute a bounded tool invocation and retain its complete diagnostic output."""
    with Path(log).open("w", encoding="utf-8") as output:
        subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                       check=True, timeout=900, **kwargs)


def verified_copy(source, destination, expected):
    """Copy an approved asset only after checking its size and SHA-256 identity."""
    source = Path(source)
    payload = source.read_bytes()
    if len(payload) != expected["size"] or sha256(source) != expected["sha256"]:
        raise ValueError(f"ROM identity mismatch: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(payload)
    return {"file": source.name, "size": len(payload), "sha256": sha256(source)}


def prepare_roms(args, output, manifest):
    """Stage approved external inputs under the pinned emulator's ROM filenames."""
    entries = {entry["part"]: entry for entry in manifest["drive_roms"]}
    _, main, controller = MODELS[args.model]
    identities = []
    for part in (*main, controller):
        identities.append(verified_copy(
            args.drive_rom_dir / (part + ".bin"),
            output / "roms" / ("c" + args.model) / (part + "." + SUFFIXES[part]),
            entries[part],
        ))
    controller_entries = {entry["file"]: entry for entry in manifest["controller_roms"]}
    for source, destination in CONTROLLERS.items():
        path = args.controller_rom_dir / source
        entry = controller_entries.get(source)
        if entry is None:
            # MAME's non-CRTC editor is an extra, explicitly pinned controller asset.
            entry = {"size": 2048,
                     "sha256": "44c79cce9be64be26000821957ef17ca2d86ab5cd66c1e6c4342b91fd5f04b67"}
        identities.append(verified_copy(path, output / "roms/cbm4032" / destination, entry))
    gcr = args.gcr_rom.read_bytes()
    if len(gcr) != 2048 or hashlib.sha1(gcr).hexdigest() != GCR_SHA1:
        raise ValueError("901467 hardware GCR lookup ROM identity mismatch")
    device = "c2040_fdc" if args.model == "4040" else "c8050fdc"
    target = output / "roms" / device / "901467.uk6"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(gcr)
    identities.append({"file": "901467", "size": len(gcr), "sha256": sha256(target)})
    return identities


def basic_program():
    """Build a bounded native PET controller covering status, EOF, TALK reuse and writes."""
    # Reserve capture/mailbox RAM above BASIC's heap (pinned PET V4R bassrc:118-124).
    lines = [
        f"5 POKE{BASIC_MEMSIZ_LOW},0:POKE{BASIC_MEMSIZ_HIGH},{CAPTURE_ADDRESS >> 8}:CLR",
        f"10 POKE{MAILBOX_ADDRESS},1:OPEN15,8,15",
        "20 B=16384:M=28673:P=1:GOSUB9000",
        "30 B=16512:M=28674:P=3:GOSUB9000",
        '40 B=16896:M=28675:P=5:F$="0:SEED,P,R":K=1:GOSUB9100',
        '50 B=21504:M=28686:P=11:F$="0:ONE,S,R":K=0:GOSUB9100',
        '60 B=18432:M=28680:P=13:F$="0:BOUND254,S,R":GOSUB9100',
        '70 B=19456:M=28682:P=15:F$="0:BOUND255,S,R":GOSUB9100',
        f'80 POKE{PHASE_ADDRESS},17:OPEN3,8,3,"0:RESULT,S,W"',
        '90 PRINT#3,"M1 ROM WRITE":CLOSE3',
        f"100 POKE{PHASE_ADDRESS},18:B=22528:M=28688:P=19:GOSUB9000",
        '110 B=20480:M=28684:P=21:F$="0:RESULT,S,R":GOSUB9100',
        '120 CLOSE15:OPEN15,8,15,"UJ"',
        "130 B=22656:M=28689:P=23:GOSUB9000:CLOSE15",
        f"140 POKE{MAILBOX_ADDRESS},{COMPLETE}:END",
        f"9000 POKE{PHASE_ADDRESS},P:J=0",
        "9010 GET#15,A$:V=ASC(A$+CHR$(0)):Z=ST:POKEB+J,V:POKEB+64+J,Z:J=J+1",
        "9020 IFV<>13ANDZ=0ANDJ<64THEN9010",
        f"9030 POKEM,J:POKE{PHASE_ADDRESS},P+1:RETURN",
        f"9100 POKE{PHASE_ADDRESS},P:OPEN2,8,2,F$:I=0",
        "9110 GET#2,A$:V=ASC(A$+CHR$(0)):T=ST:POKEB+I,V:POKEB+512+I,T:I=I+1",
        "9120 IFK=1ANDI=2THENGOSUB9200",
        "9130 IFT=0ANDI<512THEN9110",
        f"9140 POKEM,IAND255:POKEM+1,INT(I/256):CLOSE2:POKE{PHASE_ADDRESS},P+1:RETURN",
        "9200 BS=B:MS=M:PS=P:B=17920:M=28677:P=9:GOSUB9000:B=BS:M=MS:P=PS:RETURN",
    ]
    return "\n".join(lines).lower() + "\n"


def fixture_bam_entry(model, data, track):
    """Resolve native BAM links and declared ranges independently of production code."""
    offset = image_offset(model, 18 if model == "4040" else 38, 0)
    if model == "4040":
        return offset + track * 4
    for _ in range(4):
        low, high = data[offset + 4:offset + 6]
        if low <= track < high:
            return offset + 6 + (track - low) * 5
        offset = image_offset(model, data[offset], data[offset + 1])
    raise ValueError("fixture BAM has no requested track range")


def decode_capture(capture, mailbox, layout=LAYOUT):
    """Decode only completed, bounded per-case bytes and KERNAL status samples."""
    if len(capture) != 0x1c00 or len(mailbox) != 32 or mailbox[0] != COMPLETE:
        raise ValueError("reference controller did not complete its bounded cases")
    observations = {}
    for name, (data, status, count, limit) in layout.items():
        size = mailbox[count]
        if limit > 255:
            size += mailbox[count + 1] << 8
        if not 0 < size <= limit:
            raise ValueError(f"incomplete or oversized reference observation: {name}")
        start = data - CAPTURE_ADDRESS
        state = status - CAPTURE_ADDRESS
        observations[name] = {
            "bytes": list(capture[start:start + size]),
            "kernal_st": list(capture[state:state + size]),
        }
    return observations


def check_observations(model, observations):
    """Require native fixture results without treating current firmware as the oracle."""
    expected = {
        "seed": SEED, "onebyte": b"A\0\2A", "boundary254": bytes(range(254)),
        "boundary255": bytes(range(255)), "readback": WRITTEN,
    }
    for name, payload in expected.items():
        observation = observations[name]
        if bytes(observation["bytes"]) != payload:
            raise ValueError(f"native byte mismatch: {model} {name}")
    version = b"2" if model == "4040" else b"2.7"
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    startup = b"73,CBM DOS V" + version + trailer
    for name in ("startup", "reset_status"):
        if bytes(observations[name]["bytes"]) != startup:
            raise ValueError(f"native reset/status mismatch: {model} {name}")
    for name in ("consumed", "interposed", "written_status"):
        if bytes(observations[name]["bytes"]) != b"00, OK" + trailer:
            raise ValueError(f"native consumed/write status mismatch: {model} {name}")
    for name, observation in observations.items():
        if observation["kernal_st"] != [0] * (len(observation["bytes"]) - 1) + [64]:
            raise ValueError(f"native EOF/error status mismatch: {model} {name}")


def controller_commands(path):
    """Project PET ATN-tagged DIO writes, not unobserved IEEE acceptance edges."""
    atn = None
    commands = []
    with Path(path).open(encoding="ascii") as stream:
        for row in csv.DictReader(stream):
            if row["direction"] != "write":
                continue
            address, value = int(row["address"], 16), int(row["value"], 16)
            if address == PET_VIA_PORT_B:
                atn = not bool(value & PET_ATN_MASK)
            elif address == PET_PIA2_PORT_B and atn and value != BUS_RELEASED:
                commands.append({"seconds": float(row["seconds"]), "byte": value ^ BUS_RELEASED})
    return commands


def check_interruption(commands, phases):
    """Require a status TALK between source-file TALK selections on the still-open channel."""
    starts = [float(row["seconds"]) for row in phases if row["phase"] == "9"]
    ends = [float(row["seconds"]) for row in phases if row["phase"] == "10"]
    seed_starts = [float(row["seconds"]) for row in phases if row["phase"] == "5"]
    seed_ends = [float(row["seconds"]) for row in phases if row["phase"] == "6"]
    if (any(len(values) != 1 for values in (starts, ends, seed_starts, seed_ends))
            or not seed_starts[0] < starts[0] < ends[0] < seed_ends[0]):
        raise ValueError("incomplete interposed-status phase coverage")
    selections = []
    for first, second in zip(commands, commands[1:]):
        if first["byte"] == TALK_DEVICE_8:
            selections.append(second)
    before = [row for row in selections if seed_starts[0] < row["seconds"] < starts[0]]
    during = [row for row in selections if starts[0] <= row["seconds"] <= ends[0]]
    after = [row for row in selections if ends[0] < row["seconds"] < seed_ends[0]]
    if (not any(row["byte"] == DATA_SECONDARY for row in before)
            or not any(row["byte"] == STATUS_SECONDARY for row in during)
            or not any(row["byte"] == DATA_SECONDARY for row in after)):
        raise ValueError("source TALK interruption/resumption not observed")


def check_output_directory(output):
    """Prevent overwriting existing paths or staging private ROMs in tracked directories."""
    repository = Path(__file__).resolve().parents[2]
    if output.is_relative_to(repository):
        ignored = subprocess.run(
            ["git", "check-ignore", "-q", str(output)], cwd=repository, check=False,
        )
        if ignored.returncode != 0:
            raise ValueError("output inside the repository must be git-ignored (use build/)")
    output.mkdir(parents=True, exist_ok=False)


def check_machine_observation(machine, require_complete=True):
    """Require observed I/O, and reject capped logs when full capture is evidence."""
    fields = dict(line.split("=", 1) for line in machine.splitlines() if "=" in line)
    complete = fields.get("trace_complete") == "true"
    if int(fields.get("io_events", "0")) <= 0 or (require_complete and not complete):
        raise ValueError("reference controller I/O observation coverage is incomplete")
    return complete


def prepare_image(args, output):
    """Create disposable baseline files and preserve the pre-execution image."""
    extension = MODELS[args.model][0]
    initial = output / ("initial." + extension)
    run([str(args.c1541), "-format", "M1 FIXTURE,01", extension, str(initial)],
        output / "format.log")
    fixtures = {"seed.prg": SEED, "one.seq": b"A",
                "bound254.seq": bytes(range(254)), "bound255.seq": bytes(range(255))}
    for name, payload in fixtures.items():
        path = output / name
        path.write_bytes(payload)
        file_type = ",s" if name.endswith(".seq") else ",p"
        run([str(args.c1541), "-attach", str(initial), "-write", str(path),
             path.stem + file_type], output / (name + ".log"))

    # Keep the fixture self-contained, without later operator acceptance programs.
    source = output / "controller.txt"
    source.write_text(basic_program(), encoding="ascii")
    program = output / "controller.prg"
    run([str(args.petcat), "-h", "-w1p", "-l", f"{LOAD_ADDRESS:04x}", "-o", str(program),
         "--", str(source)], output / "petcat.log")
    tokenized = program.read_bytes()
    if (len(tokenized) < 7 or int.from_bytes(tokenized[:2], "little") != LOAD_ADDRESS
            or tokenized[6] != 0x97):
        raise ValueError("PET BASIC controller did not tokenize correctly")
    run([str(args.c1541), "-attach", str(initial), "-write", str(program), "m1check,p"],
        output / "controller-fixture.log")
    final = output / ("final." + extension)
    shutil.copyfile(initial, final)
    return initial, final, program


def main():
    """Capture a bounded original-ROM baseline, validating identities and coverage."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", choices=MODELS, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--drive-rom-dir", type=Path, required=True)
    parser.add_argument("--controller-rom-dir", type=Path, required=True)
    parser.add_argument("--gcr-rom", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--mame", type=Path, default=Path("/usr/games/mame"))
    parser.add_argument("--trace-build-info", type=Path)
    parser.add_argument("--c1541", type=Path, default=Path("/usr/bin/c1541"))
    parser.add_argument("--petcat", type=Path, default=Path("/usr/bin/petcat"))
    args = parser.parse_args()

    # Bind executed ROMs and composed-net instrumentation to their pinned inputs.
    build_info = None
    if args.trace_build_info:
        build_info = json.loads(args.trace_build_info.read_text(encoding="ascii"))
        if (build_info["sha256"] != sha256(args.mame)
                or build_info["upstream_commit"] != MAME_COMMIT
                or build_info["patch_sha256"] != sha256(Path(__file__).with_name("drive_reference_mame.patch"))):
            raise ValueError("unverified traced MAME build")
    elif sha256(args.mame) != MAME_SHA256:
        raise ValueError("unverified MAME binary (supply pinned trace build information)")
    output = args.output_dir.resolve()
    manifest = json.loads(args.manifest.read_text(encoding="ascii"))
    check_output_directory(output)
    identities = prepare_roms(args, output, manifest)
    initial, final, program = prepare_image(args, output)
    before = sha256(initial)

    # Observe the real PET and drive CPUs without replacing their bus routines.
    environment = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                       ECONOPET_REFERENCE_OUTPUT=str(output),
                       ECONOPET_REFERENCE_MODEL=args.model,
                       ECONOPET_REFERENCE_DEADLINE="180")
    environment.pop("ECONOPET_REFERENCE_BOARD_IMAGE", None)
    if build_info:
        environment["ECONOPET_IEEE_TRACE"] = str(output / "ieee-wire.csv")
    else:
        environment.pop("ECONOPET_IEEE_TRACE", None)
    command = [
        str(args.mame), "cbm4032", "-noreadconfig", "-bios", "basic4r",
        "-rompath", str(output / "roms"), "-ieee8", "c" + args.model,
        "-flop1", str(final), "-quik", str(program),
        "-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
        "-autoboot_delay", "5", "-autoboot_script",
        str(Path(__file__).with_suffix(".lua").resolve()),
        "-seconds_to_run", "185", "-cfg_directory", str(output / "cfg"),
        "-nvram_directory", str(output / "nvram"),
    ]
    run(command, output / "mame.log", env=environment)
    _, parts, controller = MODELS[args.model]
    expected_main = b"".join((args.drive_rom_dir / (part + ".bin")).read_bytes()
                             for part in parts)
    if (output / "active-main.bin").read_bytes() != expected_main:
        raise ValueError("executed main-DOS ROM differs from approved inputs")
    if (output / "active-controller.bin").read_bytes() != (
            args.drive_rom_dir / (controller + ".bin")).read_bytes():
        raise ValueError("executed controller ROM differs from approved input")

    # Require complete independent PET captures and the persisted write payload.
    machine = (output / "machine.txt").read_text(encoding="ascii")
    check_machine_observation(machine)
    observations = decode_capture((output / "capture.bin").read_bytes(),
                                  (output / "mailbox.bin").read_bytes())
    check_observations(args.model, observations)
    persisted = output / "persisted.seq"
    run([str(args.c1541), "-attach", str(final), "-read", "result,s", str(persisted)],
        output / "extract.log")
    if persisted.read_bytes() != WRITTEN:
        raise ValueError("persisted file mismatch")
    old, new = initial.read_bytes(), final.read_bytes()
    if sha256(initial) != before or len(old) != len(new):
        raise ValueError("immutable input or container geometry changed")
    changed = [offset // 256 for offset in range(0, len(old), 256)
               if old[offset:offset + 256] != new[offset:offset + 256]]
    if not changed:
        raise ValueError("native write did not change the persisted image")
    with (output / "phases.csv").open(encoding="ascii") as stream:
        phases = list(csv.DictReader(stream))
    commands = controller_commands(output / "controller-io.csv")
    check_interruption(commands, phases)

    # Export replay stimulus only from complete, independently cross-checked nets.
    wire = None
    if build_info:
        starts = [row["seconds"] for row in phases if row["phase"] == "0"]
        if len(starts) != 1:
            raise ValueError("missing or repeated native stimulus start marker")
        wire = decode_wire(output / "ieee-wire.csv", attoseconds(starts[0]))
        wire["domain"] = "mame-ieee488-composed"
        label_cases(wire["frames"], phases)
        check_wire_bytes(wire, observations)
        write_replay(output / "ieee-wire.csv", wire, output / "replay.txt")
    result = {
        "schema": 1, "model": args.model, "controller": "cbm4032/basic4r",
        "scenario": "ordinary-files-reset",
        "emulator": {"version": "0.285" if build_info else "0.285+dfsg1-1",
                     "upstream_commit": MAME_COMMIT, "sha256": sha256(args.mame)},
        "roms": identities, "approval_manifest_sha256": sha256(args.manifest),
        "tools": {name: sha256(getattr(args, name)) for name in ("c1541", "petcat")},
        "command": command, "observations": observations, "phases": phases,
        "controller_commands": commands, "wire": wire, "trace_build": build_info,
        "machine": machine,
        "artifacts": {name: sha256(output / name) for name in (
            "controller.prg", "controller-io.csv", "phases.csv", "capture.bin", "mailbox.bin")},
        "harness": {name: sha256(Path(__file__).with_name(name)) for name in (
            "drive_reference.py", "drive_reference.lua", "drive_wire.py")},
        "media": {"initial_sha256": before, "final_sha256": sha256(final),
                  "size": len(new), "changed_sectors": changed,
                  "persisted_file_sha256": sha256(persisted)},
        "limits": [
            "PET BASIC operation times include interpreter/controller overhead",
            "exact composed-net trace available only when wire is non-null",
            "physical timing and storage-fault parity are not established",
            "MAME FDC write-protect handling is marked TODO",
        ],
    }
    if wire:
        result["artifacts"].update({name: sha256(output / name)
                                    for name in ("ieee-wire.csv", "replay.txt")})
        result["harness"]["drive_reference_mame.patch"] = sha256(
            Path(__file__).with_name("drive_reference_mame.patch"))
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="ascii")
    print(f"{args.model}: original-ROM baseline and persisted write verified ({output})")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
