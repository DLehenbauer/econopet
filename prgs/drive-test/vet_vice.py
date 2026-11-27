#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Vet drive-test.prg in headless VICE (xpet -console -warp) against real Commodore ROMs."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "tools/drive-roms/drive-emulation-roms.json"
DEFAULT_DRIVE_ROM_CACHE = ROOT / ".tmp/ai/drive-roms/download"
DEFAULT_PRG = ROOT / "build/prgs/drive-test/bin/drive-test.prg"
DEFAULT_LABELS = ROOT / "build/prgs/drive-test/bin/drive-test.lbl"

BASIC_READY = 0xB3FF
BASIC_VARTAB = 0x002A
KEY_COUNT = 0x009E
KEY_BUFFER = 0x026F
PRG_LOAD_ADDRESS = 0x0401
MAILBOX_ADDR = 0x7FF0
MAILBOX_SIZE = 16
HOST_STANDALONE = 0
HOST_ATTACHED = 1
STATE_PASSED = 2
STATE_FAILED = 3
EXPECTED_CASES = 468

DISK_SPECS = (
    ("u8d0.d64", 174_848),
    ("u8d1.d64", 174_848),
    ("u9d0.d80", 533_248),
    ("u9d1.d80", 533_248),
    ("u10d0.d82", 1_066_496),
    ("u10d1.d82", 1_066_496),
    ("u11d0.d64", 174_848),
    ("u11d1.d64", 174_848),
)

CONTROLLER_ROMS = (
    "basic-4-b000.901465-23.bin",
    "basic-4-c000.901465-20.bin",
    "basic-4-d000.901465-21.bin",
    "edit-4-80-b-60Hz.901474-03.bin",
    "kernal-4.901465-22.bin",
    "characters-2.901447-10.bin",
)

DRIVE_4040_PARTS = ("901468-14", "901468-15", "901468-16")
DRIVE_8050_PARTS = ("901887-01", "901888-01")


def resolve_symbol(labels_text: str, symbol: str) -> int:
    """Resolve one exported C/assembly label from a CC65 .lbl file."""
    # Match CC65's 'al <hex_addr> ._<symbol>' line format and return the integer address.
    match = re.search(rf"^al ([0-9A-Fa-f]{{6}}) \._{re.escape(symbol)}$", labels_text, re.MULTILINE)
    if match is None:
        raise ValueError(f"missing symbol _{symbol} in label file")
    return int(match.group(1), 16)


def verify_sha256(path: Path, size: int, expected_sha256: str) -> bytes:
    """Read a ROM binary and verify its exact byte length and SHA-256 digest."""
    # Load the file bytes and reject any size or SHA-256 mismatch against the pinned manifest.
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != size or digest != expected_sha256:
        raise ValueError(f"ROM digest/size mismatch for {path}")
    return data


def ensure_drive_roms(manifest: dict, rom_dir: Path, download: bool) -> dict[str, bytes]:
    """Load and verify the approved 4040 and 8050/8250 main-DOS ROM parts."""
    # Index the required 4040 and 8050/8250 ROM entries from the manifest.
    needed = set(DRIVE_4040_PARTS + DRIVE_8050_PARTS)
    specs = {entry["part"]: entry for entry in manifest["drive_roms"] if entry["part"] in needed}
    if set(specs) != needed:
        raise ValueError("manifest is missing required drive ROM entries")

    # Verify each cached ROM part, optionally downloading missing parts atomically via .part files.
    rom_dir.mkdir(parents=True, exist_ok=True)
    loaded: dict[str, bytes] = {}
    for part in sorted(needed):
        spec = specs[part]
        target = rom_dir / f"{part}.bin"
        if not target.exists():
            if not download:
                raise FileNotFoundError(f"missing drive ROM {target} (pass --download-roms to fetch)")
            request = urllib.request.Request(spec["url"], headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(request, timeout=60) as response:
                raw = response.read()
            partial = target.with_suffix(".bin.part")
            partial.write_bytes(raw)
            verify_sha256(partial, spec["size"], spec["sha256"])
            partial.replace(target)
        loaded[part] = verify_sha256(target, spec["size"], spec["sha256"])
    return loaded


def load_controller_roms(manifest: dict, media_rom_dir: Path) -> dict[str, Path]:
    """Verify the PET ROM 4 controller binaries in ECONOPET_MEDIA_DIR/rom."""
    # Validate each required BASIC 4, Editor, KERNAL, and character ROM against its manifest SHA-256.
    specs = {entry["file"]: entry for entry in manifest["controller_roms"]}
    verified: dict[str, Path] = {}
    for filename in CONTROLLER_ROMS:
        spec = specs[filename]
        path = media_rom_dir / filename
        if not path.exists():
            raise FileNotFoundError(f"missing PET controller ROM: {path}")
        verify_sha256(path, spec["size"], spec["sha256"])
        verified[filename] = path
    return verified


def write_monitor_scripts(
    workspace: Path,
    prg_path: Path,
    prg_end: int,
    fail_halt_addr: int,
) -> Path:
    """Write VICE monitor scripts that load, run, and capture completion or failure."""
    start = workspace / "start.mon"
    run = workspace / "run.mon"
    finish = workspace / "finish.mon"

    # Script 3 (finish.mon): dump the $7FF0 mailbox to result.bin, print the 80x25 screen, and exit VICE.
    finish.write_text(
        f'bsave "{workspace / "result.bin"}" 0 ${MAILBOX_ADDR:04x} ${MAILBOX_ADDR + MAILBOX_SIZE - 1:04x}\n'
        "screen\n"
        "quit\n",
        encoding="ascii",
    )

    # Seed a prior attached pass so standalone LOAD/RUN must reject stale attachment and return to BASIC READY.
    prior_mailbox = (
        b"TD" + bytes((HOST_ATTACHED, STATE_PASSED))
        + EXPECTED_CASES.to_bytes(2, "little") + bytes(MAILBOX_SIZE - 6)
    )
    run.write_text(
        "delete\n"
        f"> ${MAILBOX_ADDR:04x} {prior_mailbox.hex(' ')}\n"
        f'load "{prg_path}" 0\n'
        f"> ${BASIC_VARTAB:04x} {prg_end & 0xff:02x} {(prg_end >> 8) & 0xff:02x}\n"
        f"break ${fail_halt_addr:04x}\n"
        f'command 1 "playback \\"{finish}\\""\n'
        f"break ${BASIC_READY:04x}\n"
        f"condition 2 if @ram:${MAILBOX_ADDR + 3:04x} >= ${STATE_PASSED:02x}\n"
        f'command 2 "playback \\"{finish}\\""\n'
        f"> ${KEY_BUFFER:04x} 52 55 4e 0d\n"
        f"> ${KEY_COUNT:04x} 04\n"
        "g\n",
        encoding="ascii",
    )

    # Script 1 (start.mon): break on the initial boot entry into BASIC READY ($B3FF) to trigger run.mon.
    start.write_text(
        f"break ${BASIC_READY:04x}\n"
        f'command 1 "playback \\"{run}\\""\n'
        "g\n",
        encoding="ascii",
    )
    return start


def decode_result(raw_bsave: bytes) -> dict[str, int]:
    """Decode the 16-byte $7FF0 target diagnostic mailbox from a VICE bsave file."""
    # Strip VICE bsave's 2-byte load-address prefix when present and unpack the mailbox fields.
    if len(raw_bsave) < MAILBOX_SIZE:
        raise ValueError(f"truncated mailbox capture ({len(raw_bsave)} bytes)")
    payload = raw_bsave[2:] if len(raw_bsave) >= MAILBOX_SIZE + 2 else raw_bsave
    return {
        "magic0": payload[0],
        "magic1": payload[1],
        "host_control": payload[2],
        "state": payload[3],
        "completed": int.from_bytes(payload[4:6], "little"),
        "detail_id": payload[6],
        "expected": payload[7],
        "actual": payload[8],
        "aux0": payload[9],
        "aux1": payload[10],
        "offset": int.from_bytes(payload[11:13], "little"),
    }


def extract_screen(monitor_log: str) -> str:
    """Return the final 80x25 screen dump from a VICE monitor log."""
    # Locate the last 'screen' command header in the monitor log, falling back to trailing log lines.
    marker = "Displaying 80x25 screen"
    pos = monitor_log.rfind(marker)
    if pos < 0:
        return "\n".join(monitor_log.splitlines()[-30:])
    lines = monitor_log[pos:].splitlines()[1:26]
    return "\n".join(line.rstrip() for line in lines)


def drive_arguments(disks: dict[str, Path]) -> list[str]:
    """Configure true-drive units and their paired scratch images in VICE argument order."""
    options = []
    attachments = []
    for device, model, extension in (
        (8, "4040", "d64"),
        (9, "8050", "d80"),
        (10, "8250", "d82"),
        (11, "4040", "d64"),
    ):
        options.extend([
            f"-drive{device}type", model,
            f"-drive{device}truedrive",
            f"-drive{device}idle", "0",
            f"+virtualdev{device}",
        ])
        for drive in range(2):
            flag = f"-{device}" if drive == 0 else f"-{device}d1"
            attachments.extend([flag, str(disks[f"u{device}d{drive}.{extension}"])])
    return options + attachments


def run_vice(args: argparse.Namespace) -> int:
    """Execute drive-test.prg in headless xpet -warp and validate completion."""
    # Validate the PRG load address, resolve _drive_fail_halt, and verify all PET and drive ROM images.
    prg_path = args.prg.resolve()
    labels_path = args.labels.resolve()
    manifest = json.loads(args.manifest.read_text(encoding="ascii"))

    prg_bytes = prg_path.read_bytes()
    if len(prg_bytes) < 3 or int.from_bytes(prg_bytes[:2], "little") != PRG_LOAD_ADDRESS:
        raise ValueError(f"invalid PET PRG header in {prg_path}")
    prg_end = PRG_LOAD_ADDRESS + len(prg_bytes) - 2

    labels_text = labels_path.read_text(encoding="ascii")
    fail_halt_addr = resolve_symbol(labels_text, "drive_fail_halt")

    controller = load_controller_roms(manifest, args.media_dir.resolve() / "rom")
    drive_roms = ensure_drive_roms(manifest, args.drive_rom_dir.resolve(), args.download_roms)

    with tempfile.TemporaryDirectory(prefix="drive-test-vice-") as temp_name:
        workspace = Path(temp_name)

        # Concatenate multi-chip BASIC 4, 4040 DOS 2.0, and 8050/8250 DOS 2.7 ROM images expected by xpet.
        basic_rom = workspace / "basic4.bin"
        basic_rom.write_bytes(
            controller["basic-4-b000.901465-23.bin"].read_bytes()
            + controller["basic-4-c000.901465-20.bin"].read_bytes()
            + controller["basic-4-d000.901465-21.bin"].read_bytes()
        )
        dos4040_rom = workspace / "dos4040.bin"
        dos4040_rom.write_bytes(b"".join(drive_roms[part] for part in DRIVE_4040_PARTS))
        dos8050_rom = workspace / "dos8050.bin"
        dos8050_rom.write_bytes(b"".join(drive_roms[part] for part in DRIVE_8050_PARTS))

        # Create eight zero-filled D64/D80/D82 scratch containers for true-drive units 8..11.
        disks: dict[str, Path] = {}
        for name, size in DISK_SPECS:
            target = workspace / name
            target.write_bytes(bytes(size))
            disks[name] = target

        # Generate monitor automation scripts and launch headless xpet in warp mode with true-drive emulation.
        monitor_start = write_monitor_scripts(workspace, prg_path, prg_end, fail_halt_addr)
        monitor_log = workspace / "monitor.txt"
        command = [
            args.vice,
            "-default",
            "-console",
            "-warp",
            "+sound",
            "-model",
            "8032",
            "-limitcycles",
            str(args.limit_cycles),
            "-jamaction",
            "5",
            "-initbreak",
            "reset",
            "-kernal",
            str(controller["kernal-4.901465-22.bin"]),
            "-editor",
            str(controller["edit-4-80-b-60Hz.901474-03.bin"]),
            "-chargen",
            str(controller["characters-2.901447-10.bin"]),
            "-basic",
            str(basic_rom),
            "-dos4040",
            str(dos4040_rom),
            "-dos1001",
            str(dos8050_rom),
            *drive_arguments(disks),
            "-moncommands",
            str(monitor_start),
            "-monlog",
            "-monlogname",
            str(monitor_log),
        ]

        started = time.monotonic()
        completed = subprocess.run(command, cwd=workspace, capture_output=True, text=True, timeout=args.timeout)
        elapsed = time.monotonic() - started
        log_text = monitor_log.read_text(encoding="utf-8", errors="replace") if monitor_log.exists() else ""
        screen = extract_screen(log_text)

        # Fail if xpet exited abnormally or never reached the finish.mon mailbox dump.
        result_path = workspace / "result.bin"
        if completed.returncode != 0 or not result_path.exists():
            print(f"VICE exited with code {completed.returncode} after {elapsed:.2f}s", file=sys.stderr)
            if screen:
                print(screen, file=sys.stderr)
            if completed.stderr:
                print("\n".join(completed.stderr.splitlines()[-20:]), file=sys.stderr)
            return 1

        # Decode the captured $7FF0 mailbox, print the final PET screen, and require all 468 checks to pass.
        result = decode_result(result_path.read_bytes())
        print(screen)
        print(
            f"\nVICE result ({elapsed:.2f}s): magic={result['magic0']:02X}{result['magic1']:02X} "
            f"state={result['state']} completed={result['completed']} "
            f"detail={result['detail_id']} offset={result['offset']} "
            f"expected=${result['expected']:02X} actual=${result['actual']:02X}"
        )
        if (
            result["magic0"] != ord("T")
            or result["magic1"] != ord("D")
            or result["host_control"] != HOST_STANDALONE
            or result["state"] != STATE_PASSED
            or result["completed"] != EXPECTED_CASES
        ):
            return 1
        return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse CLI options for headless VICE vetting."""
    media_default = os.environ.get("ECONOPET_MEDIA_DIR", "")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prg", type=Path, default=DEFAULT_PRG, help="Built drive-test.prg path")
    parser.add_argument("--labels", type=Path, default=DEFAULT_LABELS, help="CC65 .lbl file for drive-test.prg")
    parser.add_argument(
        "--media-dir",
        type=Path,
        default=Path(media_default) if media_default else ROOT / "sdcard",
        help="Media root containing rom/ (defaults to $ECONOPET_MEDIA_DIR)",
    )
    parser.add_argument("--drive-rom-dir", type=Path, default=DEFAULT_DRIVE_ROM_CACHE, help="Directory of drive ROMs")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="ROM hash manifest JSON")
    parser.add_argument("--vice", default="xpet", help="xpet executable")
    parser.add_argument("--download-roms", action="store_true", help="Download missing drive ROMs from manifest URLs")
    parser.add_argument("--limit-cycles", type=int, default=2_000_000_000, help="Emulated CPU cycle ceiling")
    parser.add_argument("--timeout", type=int, default=180, help="Wall-clock timeout in seconds")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    try:
        return run_vice(parse_args(argv))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"VICE vetting failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
