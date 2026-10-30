#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Compare original 4040 controller-ROM jobs and persisted media with production C jobs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import zipfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/drive-ref"))
from drive_reference import prepare_roms

IMAGE_SIZE = 174848
SECTOR_SIZE = 256
READ, WRITE, VERIFY, SEEK, BUMP, FORMAT = 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xe0


def image(drive):
    """Construct independent patterned sectors with a valid physical disk-ID source."""
    data = bytearray((i // SECTOR_SIZE + i % SECTOR_SIZE + drive * 17) & 255
                     for i in range(IMAGE_SIZE))
    bam = 17 * 21 * SECTOR_SIZE
    # MAME 0.264's static D64 sector descriptor captures the first loaded ID.
    # Equal IDs avoid that reference bug. Distinct sector data still tests routing.
    data[bam + 0xa2:bam + 0xa4] = b"ID"
    return data


def requests(scenario="normal"):
    """Cover both drives, every slot, density-zone boundaries and persistent job sequences."""
    records = []

    def job(name, slot, operation, drive, track, sector, payload=None, wrong_id=False):
        """Preserve distinct unused-header bytes so silent clobbering becomes observable."""
        header = bytes((ord("X") if wrong_id else ord("I"), ord("D"),
                        track, sector, 0x59, 0x6a, 0x7b, 0x8c))
        if payload is None:
            payload = bytes((byte ^ 0xa5 ^ slot ^ drive) & 255 for byte in range(SECTOR_SIZE))
        records.append(f"{name} {slot} {operation | drive} {header.hex()} {payload.hex()}")

    if scenario == "format":
        job("format0", 0, FORMAT, 0, 1, 0)
        job("format_seek0", 1, SEEK, 0, 18, 0)
        job("format_read0", 2, READ, 0, 1, 0)
        return "\n".join(records) + "\n"
    if scenario == "errors":
        for drive in range(2):
            job(f"bad_sector{drive}", 0, READ, drive, 1, 21)
            job(f"bad_track{drive}", 1, READ, drive, 36, 0)
            job(f"zero_track{drive}", 2, READ, drive, 0, 0)
            job(f"bad_sector_id{drive}", 3, READ, drive, 1, 21, wrong_id=True)
        return "\n".join(records) + "\n"
    if scenario != "normal":
        for drive in range(2):
            job(f"{scenario}_bump{drive}", 0, BUMP, drive, 0, 0)
            job(f"{scenario}_seek{drive}", 1, SEEK, drive, 18, 0)
            job(f"{scenario}_read{drive}", 2, READ, drive, 1, 0)
            job(f"{scenario}_write{drive}", 3, WRITE, drive, 1, 0)
            job(f"{scenario}_verify{drive}", 4, VERIFY, drive, 1, 0)
        if scenario == "protected":
            job("protected_format0", 0, FORMAT, 0, 1, 0)
        return "\n".join(records) + "\n"

    # Native BUMP/SEEK establish per-drive mechanics without prescribing their output.
    for drive in range(2):
        job(f"bump{drive}", 0, BUMP, drive, 0, 0)
        job(f"seek{drive}", 0, SEEK, drive, 18, 0)
        for slot in range(15):
            job(f"read{drive}_{slot}", slot, READ, drive, 1, slot)
            job(f"write{drive}_{slot}", slot, WRITE, drive, 1, slot)
            job(f"verify{drive}_{slot}", slot, VERIFY, drive, 1, slot)
            job(f"readback{drive}_{slot}", slot, READ, drive, 1, slot)
        for track in (17, 18, 24, 25, 30, 31, 35):
            sectors = 21 if track <= 17 else 19 if track <= 24 else 18 if track <= 30 else 17
            job(f"zone{drive}_{track}", track % 15, READ, drive, track, sectors - 1)
        job(f"verify_mismatch{drive}", 0, VERIFY, drive, 1, 0, bytes(SECTOR_SIZE))
        job(f"id_mismatch{drive}", 0, READ, drive, 1, 0, wrong_id=True)
    return "\n".join(records) + "\n"


def run(command, root, log, **kwargs):
    """Retain complete bounded subprocess diagnostics for reproducible failures."""
    with (root / log).open("w", encoding="utf-8") as output:
        subprocess.run(command, cwd=root, stdout=output, stderr=subprocess.STDOUT,
                       timeout=180, check=True, **kwargs)


def parse_observations(request, response):
    """Reject incomplete, reordered, malformed or oversized native and production captures."""
    source = request.splitlines()
    records = response.splitlines()
    if len(records) != len(source) + 1 or records[-1:] != ["DONE"]:
        raise ValueError("controller observations are incomplete or have extra records")
    parsed = []
    for line, (before, after) in enumerate(zip(source, records), 1):
        expected, actual = before.split(), after.split()
        if (len(actual) != 5 or actual[:2] != expected[:2]
                or not actual[2].isdecimal() or not 0 <= int(actual[2]) < 128
                or len(actual[3]) != 16 or len(actual[4]) != SECTOR_SIZE * 2):
            raise ValueError(f"invalid controller observation at line {line}")
        bytes.fromhex(actual[3])
        bytes.fromhex(actual[4])
        parsed.append(actual)
    return parsed


def compare_observations(request, native, production):
    """Compare every observed byte except independently validated rotational SEEK choices."""
    reference = parse_observations(request, native)
    actual = parse_observations(request, production)
    differences = []
    for i, (record, before, after) in enumerate(zip(request.splitlines(), reference, actual), 1):
        source = record.split()
        if int(source[2]) & 0xfe == SEEK and before[2] == after[2] == "1":
            # SEEK reports whichever valid sector rotates past the head next.
            # Validate both headers independently, then ignore only that sector and its XOR.
            for observation in (before, after):
                header = bytearray.fromhex(observation[3])
                track = bytes.fromhex(source[3])[2]
                count = 21 if track <= 17 else 19 if track <= 24 else 18 if track <= 30 else 17
                if (header[:2] != b"ID" or header[2] != track or header[3] >= count
                        or header[4] != header[0] ^ header[1] ^ header[2] ^ header[3]):
                    raise ValueError(f"invalid physical SEEK header at request {i}")
                header[3:5] = bytes(2)
                observation[3] = header.hex()
        if before != after:
            differences.append({"index": i, "native": native.splitlines()[i - 1],
                                "production": production.splitlines()[i - 1]})
    return differences


def main():
    """Stage hash-verified ROMs, capture native jobs, replay production and compare exact outputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--replay", type=Path, required=True)
    parser.add_argument("--rom-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--scenario", choices=("normal", "protected", "absent", "errors", "format"),
                        default="normal")
    args = parser.parse_args()
    if args.rom_dir is None:
        media = os.environ.get("ECONOPET_MEDIA_DIR")
        if not media:
            parser.error("--rom-dir or ECONOPET_MEDIA_DIR is required for native-controller evidence")
        args.rom_dir = Path(media) / "roms"
    if args.output_dir:
        root = args.output_dir.resolve()
        root.mkdir(parents=True, exist_ok=False)
    else:
        artifacts = Path.cwd() / "controller-reference-artifacts"
        artifacts.mkdir(exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix=args.scenario + "-", dir=artifacts))
    print(f"Controller reference artifacts: {root}", flush=True)
    manifest = json.loads((ROOT / "tools/drive-roms/drive-emulation-roms.json").read_text())
    identities = prepare_roms(SimpleNamespace(
        model="4040", drive_rom_dir=args.rom_dir, controller_rom_dir=args.rom_dir,
        gcr_rom=args.rom_dir / "901467.uk6"), root, manifest)
    shutil.copyfile(args.rom_dir / "901467.uk6", root / "roms/c4040/901467.uk6")
    for part in ("901468-14.bin", "901468-15.bin", "901468-16.bin"):
        shutil.copyfile(args.rom_dir / part, root / part)
    for drive in range(2):
        (root / f"initial{drive}.d64").write_bytes(image(drive))
        (root / f"reference{drive}.d64").write_bytes(image(drive))
    (root / "mode.txt").write_text(
        "0 0 1 1\n" if args.scenario == "protected" else
        "1 1 1 0\n" if args.scenario == "absent" else "1 1 1 1\n")
    request = requests(args.scenario)
    (root / "requests.txt").write_text(request, encoding="ascii")
    mount = []
    for drive in range(2):
        if args.scenario == "absent" and drive == 1:
            continue
        path = root / f"reference{drive}.d64"
        if args.scenario == "protected":
            # Archive-backed media is physically write-protected to MAME even under root.
            archive = root / f"protected{drive}.zip"
            with zipfile.ZipFile(archive, "w") as zipped:
                zipped.write(path, path.name)
            path = archive
        mount.extend((f"-flop{drive + 1}", str(path)))
    script = Path(__file__).with_suffix(".lua")
    mame = args.mame.resolve()
    run([str(mame), "cbm4032", "-noreadconfig", "-rompath", str(root / "roms"),
         "-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
         *mount,
         "-autoboot_delay", "0", "-autoboot_script", str(script),
         "-seconds_to_run", "240"], root, "mame.log",
        env={**os.environ, "ECONOPET_CONTROLLER_REFERENCE": str(root)})
    run([str(args.replay.resolve()), str(root)], root, "production.log")
    native = (root / "observations.txt").read_text()
    production = (root / "production.txt").read_text()
    expected_count = len(request.splitlines()) + 1
    differences = compare_observations(request, native, production)
    media_equal = [(root / f"reference{drive}.d64").read_bytes()
                   == (root / f"production{drive}.d64").read_bytes() for drive in range(2)]
    result = {
        "reference": subprocess.check_output([str(mame), "-help"], text=True).splitlines()[0],
        "binary_sha256": hashlib.sha256(mame.read_bytes()).hexdigest(),
        "script_sha256": hashlib.sha256(script.read_bytes()).hexdigest(),
        "requests_sha256": hashlib.sha256(request.encode("ascii")).hexdigest(),
        "observations_sha256": hashlib.sha256((root / "observations.txt").read_bytes()).hexdigest(),
        "production_sha256": hashlib.sha256((root / "production.txt").read_bytes()).hexdigest(),
        "roms": identities,
        "jobs": expected_count - 1,
        "scenario": args.scenario,
        "differences": differences,
        "media_equal": media_equal,
        "complete": True,
        "comparison": "completion, full header/buffer and final media (not elapsed time)",
    }
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    if differences or not all(media_equal) or not result["complete"]:
        raise RuntimeError(f"controller reference mismatch: {len(differences)} jobs, media={media_equal}")
    print(f"Original 6504 controller agreement: {result['jobs']} jobs, both media byte-exact")


if __name__ == "__main__":
    main()
