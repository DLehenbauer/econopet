#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Extract the TPUG Waterloo disassembly disks from an ISO and create readable copies.
# Usage: ./tools/waterloo-rom-src/extract.sh TPUG.iso DESTINATION

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 TPUG.iso DESTINATION" >&2
    exit 2
fi

for tool in bsdtar c1541 python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Required tool not found: $tool" >&2
        exit 1
    fi
done

ISO="$(realpath "$1")"
if [[ ! -f "$ISO" ]]; then
    echo "ISO not found: $ISO" >&2
    exit 1
fi

mkdir -p "$2"
DESTINATION="$(cd "$2" && pwd -P)"
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TEMP_DIR"' EXIT

for path in "$DESTINATION/raw/t8" "$DESTINATION/raw/t9" \
    "$DESTINATION/raw/ta" "$DESTINATION/src"; do
    if [[ -e "$path" ]]; then
        echo "Output already exists: $path" >&2
        exit 1
    fi
done
mkdir -p "$DESTINATION/raw/"{t8,t9,ta} "$DESTINATION/src"

bsdtar -tf "$ISO" > "$TEMP_DIR/iso-members"

for disk in t8 t9 ta; do
    filename="(s)$disk.d64"
    mapfile -t members < <(
        awk -v filename="$filename" '
            {
                path = tolower($0)
                sub(/;[0-9]+$/, "", path)
                suffix = tolower("PET/S-Super PET/" filename)
                if (path == suffix ||
                    (length(path) > length(suffix) &&
                     substr(path, length(path) - length(suffix) + 1) == suffix &&
                     substr(path, length(path) - length(suffix), 1) == "/")) {
                    print $0
                }
            }
        ' "$TEMP_DIR/iso-members"
    )

    if [[ ${#members[@]} -ne 1 ]]; then
        echo "Expected one ISO member for PET/S-Super PET/$filename, found ${#members[@]}" >&2
        exit 1
    fi
    bsdtar -xOf "$ISO" "${members[0]}" > "$TEMP_DIR/$filename"
    c1541 -attach "$TEMP_DIR/$filename" \
        -cd "$DESTINATION/raw/$disk" -extract >/dev/null 2>&1 || true
done

python3 - "$DESTINATION" <<'PY'
from pathlib import Path
import sys

destination = Path(sys.argv[1])
expected_counts = {
    "t8": {"OPSYSDIS*": 3, "MAP*": 3, "NOTE_FROM_*": 2, "*.MACRO": 3, "LIBRARY.MAC": 1},
    "t9": {"OPSYSDIS*": 14},
    "ta": {"OPSYSDIS*": 18},
}

for disk, patterns in expected_counts.items():
    for pattern, expected in patterns.items():
        sources = sorted(
            source for source in (destination / "raw" / disk).glob(pattern)
            if source.is_file()
        )
        if len(sources) != expected:
            raise SystemExit(
                f"Expected {expected} {pattern} files in "
                f"{destination / 'raw' / disk}, found {len(sources)}"
            )

        for source in sources:
            output = destination / "src" / (source.name + ".txt")
            if output.exists():
                raise SystemExit(f"Duplicate source filename would overwrite {output}")
            text = source.read_bytes().replace(b"\x13\x7f", b"").replace(b"\r", b"\n")
            output.write_text(text.decode("ascii"), encoding="ascii")
PY
