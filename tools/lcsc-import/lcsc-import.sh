#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Imports an LCSC component into the project KiCad library using easyeda2kicad.
# The symbol, footprint, and 3D model are saved under hw/rev-b/mainboard/kicad/libs/LCSC.
#
# The 3D model path stored in the footprint uses ${KIPRJMOD} so that KiCad
# resolves it relative to the .kicad_pro file.
#
# Usage:
#   tools/lcsc-import/lcsc-import.sh <LCSC_ID> [<LCSC_ID> ...]
#
# Examples:
#   tools/lcsc-import/lcsc-import.sh C2040
#   tools/lcsc-import/lcsc-import.sh C2040 C2286 C1532

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
KICAD_PROJECT_DIR="$SCRIPT_DIR/../../hw/rev-b/mainboard/kicad"
LIB_NAME="LCSC"
LIB_DIR="$KICAD_PROJECT_DIR/libs/$LIB_NAME"

if [[ $# -eq 0 ]]; then
    echo "Usage: $0 <LCSC_ID> [<LCSC_ID> ...]"
    echo "Example: $0 C2040"
    exit 1
fi

# Activate the virtual environment (run setup.sh first if it does not exist)
if [[ ! -d "$VENV_DIR" ]]; then
    echo "Virtual environment not found. Running setup.sh ..."
    bash "$SCRIPT_DIR/setup.sh"
fi

# shellcheck source=/dev/null
source "$VENV_DIR/bin/activate"

# Create the library directory if it does not exist
mkdir -p "$LIB_DIR"

for LCSC_ID in "$@"; do
    # Normalize: strip leading whitespace and ensure uppercase C prefix
    LCSC_ID="$(echo "$LCSC_ID" | sed 's/^[[:space:]]*//' | tr '[:lower:]' '[:upper:]')"

    if [[ ! "$LCSC_ID" =~ ^C[0-9]+$ ]]; then
        echo "Error: '$LCSC_ID' is not a valid LCSC part number (expected format: C1234)"
        exit 1
    fi

    echo "Importing $LCSC_ID into $LIB_DIR ..."

    # Run from the KiCad project directory so that --project-relative produces
    # a correct ${KIPRJMOD}/libs/LCSC.3dshapes/... path in the footprint.
    pushd "$KICAD_PROJECT_DIR" > /dev/null
    easyeda2kicad \
        --full \
        --lcsc_id="$LCSC_ID" \
        --output "$LIB_DIR/$LIB_NAME" \
        --overwrite \
        --project-relative
    popd > /dev/null

    echo "Done: $LCSC_ID"
done

echo ""
echo "Library files:"
echo "  Symbol:    $LIB_DIR/$LIB_NAME.kicad_sym"
echo "  Footprint: $LIB_DIR/$LIB_NAME.pretty/"
echo "  3D Models: $LIB_DIR/$LIB_NAME.3dshapes/"
