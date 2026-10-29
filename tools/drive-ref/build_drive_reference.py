#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Build pinned Debug PET-only MAME with IEEE tracing and single-cycle interleaving."""

import argparse
import json
from pathlib import Path
import subprocess

from drive_reference import MAME_COMMIT, sha256


BUILD_FLAGS = [
    "SUBTARGET=pet", "SOURCES=src/mame/commodore/pet.cpp", "DEBUG=1", "SYMBOLS=1",
    "OPTIMIZE=0", "USE_QTDEBUG=0", "NO_OPENGL=1", "NO_X11=1",
    "NO_USE_XINPUT=1", "REGENIE=1",
    "NO_USE_PORTAUDIO=1", "NO_USE_PULSEAUDIO=1",
]


def main():
    """Validate source/patch identity, build without disabling assertions, and pin the result."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    source = args.source_dir.resolve()
    patch = Path(__file__).with_name("drive_reference_mame.patch").resolve()
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                     text=True).strip()
    if commit != MAME_COMMIT or not 1 <= args.jobs <= 8:
        raise ValueError("requires the pinned source and 1..8 bounded build jobs")
    expected = patch.read_bytes()
    actual = subprocess.check_output(["git", "-C", str(source), "diff", "--binary"])
    if not actual:
        subprocess.run(["git", "-C", str(source), "apply", str(patch)], check=True)
    elif actual != expected:
        raise ValueError("MAME source changes differ from the pinned trace/precision patch")
    command = ["make", *BUILD_FLAGS, f"-j{args.jobs}"]
    subprocess.run(command, cwd=source, check=True)
    binary = source / "petd"
    identity = {
        "upstream_commit": commit, "patch_sha256": sha256(patch),
        "sha256": sha256(binary), "command": command,
        "compiler": subprocess.check_output(["g++", "--version"], text=True).splitlines()[0],
        "scheduler": "perfect quantum from the 1 MHz PET CPU minimum cycle",
    }
    binary.with_suffix(".json").write_text(json.dumps(identity, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
