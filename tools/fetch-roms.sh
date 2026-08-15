#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Fetches the zimmers.net PET ROM images referenced by sdcard/config.yaml
# into a local directory for use as ECONOPET_ROMS_DIR. The ColourPET and
# ROM 1.0 disk-support images come from other sources (see sdcard/ROMS.md)
# and must be obtained separately. Existing files are kept.
#
# Usage: tools/fetch-roms.sh [dest-dir]   (default: $ECONOPET_ROMS_DIR, else ./roms)

set -euo pipefail

DEST="${1:-${ECONOPET_ROMS_DIR:-roms}}"
BASE="https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet"

PET_ROMS=(
    basic-2-c000.901465-01.bin
    basic-2-d000.901465-02.bin
    basic-4-b000.901465-23.bin
    basic-4-c000.901465-20.bin
    basic-4-d000.901465-21.bin
    characters-1.901447-08.bin
    characters-2.901447-10.bin
    edit-2-b.901474-01.bin
    edit-2-n.901447-24.bin
    edit-4-40-n-50Hz.901498-01.bin
    edit-4-40-n-60Hz.901499-01.bin
    edit-4-80-b-50Hz.901474-04-3681.bin
    edit-4-80-b-60Hz.901474-03.bin
    kernal-2.901465-03.bin
    kernal-4.901465-22.bin
    rom-1-c000.901439-01.bin
    rom-1-c800.901439-05.bin
    rom-1-d000.901439-02.bin
    rom-1-d800.901439-06.bin
    rom-1-e000.901439-03.bin
    rom-1-f000.901439-04.bin
    rom-1-f800.901439-07.bin
)

mkdir -p "$DEST"

for rom in "${PET_ROMS[@]}"; do
    if [ -f "$DEST/$rom" ]; then
        echo "have  $rom"
    else
        echo "fetch $rom"
        curl -fsS -o "$DEST/$rom" "$BASE/$rom"
    fi
done

echo "Not fetched (no zimmers.net source, see sdcard/ROMS.md):"
echo "  rom1diskrom_v15.bin colourpet-n40.bin colourpet-b40.bin"
echo "ROMs in $DEST"
