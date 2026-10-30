#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Compare production RIOT bus semantics with a separately executed MAME 6532."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def stimulus():
    """Cover port latches, edge controls and stable timer load/expiration flags independently."""
    records = []

    def write(reg, value):
        """Append a peripheral write, preserving its original order."""
        records.append(f"W {reg} {value}")

    def read(reg, mask=255):
        """Ask the reference to observe a register without prescribing its answer."""
        records.append(f"R {reg} {mask}")

    # Compare driven bits only, since the Atari and 4040 boards have different input wiring.
    for port in (0, 2):
        for ddr in range(256):
            write(port + 1, ddr)
            for value in (0, 0x55, 0xaa, 255):
                write(port, value)
                for mirror in (0, 8, 16, 24):
                    read(port + mirror, ddr)
                    read(port + 1 + mirror)
        write(port + 1, 0)

    # Generate both PA7 polarities using output mode, independent of board input switches.
    write(1, 255)
    for reg in (4, 5, 6, 7, 12, 13, 14, 15):
        for value in (0, 255):
            write(reg, value)
            write(0, 0 if reg & 1 else 128)
            read(5, 64)
            write(0, 128 if reg & 1 else 0)
            for flags in (5, 7, 13, 15, 21, 23, 29, 31):
                read(flags, 64)
            write(0, 128 if reg & 1 else 0)
            read(5, 64)

    # Compare stable flags after expiration, never timer phase or relative implementation speed.
    for load in (20, 21, 22, 23, 28, 29, 30, 31):
        write(load, 255)
        read(5, 128)
        records.append("WAIT")
        read(5, 128)
        read(31, 128)
        read(12, 0)
        # A /1 timer can expire again during CPU replay. Do not equate its phase
        # with MAME's zero-time Lua accesses. A fresh load gives a stable flag.
        write(load, 255)
        read(5, 128)
    return "\n".join(records) + "\n"


def validate_observations(request, response):
    """Reject missing, reordered, extra or malformed records before comparing implementation values."""
    expected = request.splitlines()
    actual = response.splitlines()
    if not actual or actual[-1] != "DONE" or len(actual) != len(expected) + 1:
        raise ValueError("independent RIOT reference is incomplete or has extra records")
    for line, (before, after) in enumerate(zip(expected, actual[:-1]), 1):
        source, observed = before.split(), after.split()
        if source[0] == "R":
            valid = (len(observed) == 4 and observed[0] == "R"
                     and source[1] == observed[1] and source[2] == observed[3]
                     and observed[2].isdecimal() and 0 <= int(observed[2]) <= 255)
        else:
            valid = source == observed
        if not valid:
            raise ValueError(f"independent RIOT transaction differs at line {line}: {after}")


def main():
    """Run the independent model and reject missing, truncated or mismatched observations."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--replay", type=Path, required=True)
    args = parser.parse_args()
    version = subprocess.check_output([str(args.mame), "-help"], text=True).splitlines()[0]
    script = Path(__file__).with_suffix(".lua")
    with tempfile.TemporaryDirectory(prefix="econopet-riot-") as directory:
        root = Path(directory)
        # A self-looping synthetic Atari cartridge permits RIOT testing without external ROMs.
        cartridge = bytearray(4096)
        cartridge[:4] = bytes((0x78, 0x4c, 1, 0xf0))
        cartridge[-4:-2] = bytes((0, 0xf0))
        (root / "fixture.bin").write_bytes(cartridge)
        request = stimulus()
        (root / "stimulus.txt").write_text(request, encoding="ascii")
        command = [
            str(args.mame), "a2600", "-noreadconfig", "-cart", str(root / "fixture.bin"),
            "-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
            "-autoboot_delay", "0", "-autoboot_script", str(script.resolve()),
            "-seconds_to_run", "30",
        ]
        result = subprocess.run(
            command, cwd=root, env={**os.environ, "ECONOPET_RIOT_REFERENCE": directory},
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60, check=False,
        )
        if result.returncode:
            raise RuntimeError(f"independent MAME failed ({result.returncode}):\n{result.stdout}")
        observed = root / "observations.txt"
        if not observed.exists():
            raise RuntimeError("independent RIOT reference did not complete:\n" + result.stdout)
        validate_observations(request, observed.read_text(encoding="ascii"))
        subprocess.run([str(args.replay.resolve()), str(observed)], timeout=30, check=True)
        print(json.dumps({
            "reference": version,
            "binary_sha256": hashlib.sha256(args.mame.read_bytes()).hexdigest(),
            "script_sha256": hashlib.sha256(script.read_bytes()).hexdigest(),
            "stimulus_sha256": hashlib.sha256(request.encode("ascii")).hexdigest(),
            "observations_sha256": hashlib.sha256(observed.read_bytes()).hexdigest(),
            "comparison": "register values/side effects, not elapsed time",
        }, sort_keys=True))


if __name__ == "__main__":
    main()
