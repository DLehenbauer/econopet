#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Verify external ROM identities and execute the bounded 4040 investigation."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    """Run both probes only with the approved unmodified revision-2 chips."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    manifest = json.loads((root / "tools/drive-roms/drive-emulation-roms.json").read_text())

    # Bind execution to the existing approved ROM manifest, without redistributing bytes.
    chips = []
    for part in ("901468-14", "901468-15", "901468-16"):
        expected = next(entry for entry in manifest["drive_roms"] if entry["part"] == part)
        path = args.rom_dir / (part + ".bin")
        data = path.read_bytes()
        if len(data) != expected["size"] or hashlib.sha256(data).hexdigest() != expected["sha256"]:
            raise ValueError(f"unapproved ROM: {path}")
        chips.append(data)
        print(f"{part}: SHA-256 verified", flush=True)

    # Keep the temporary concatenation outside tracked source and delete it on exit.
    with tempfile.TemporaryDirectory(prefix="ieee-6502-") as directory:
        combined = Path(directory) / "dos4040.bin"
        combined.write_bytes(b"".join(chips))
        subprocess.run([str(args.build_dir.resolve() / "rom4040_probe"), str(combined)],
                       check=True, timeout=30)
        subprocess.run([str(args.build_dir.resolve() / "fifo4040_probe"),
                        str(args.rom_dir.resolve())], check=True, timeout=30)


if __name__ == "__main__":
    main()
