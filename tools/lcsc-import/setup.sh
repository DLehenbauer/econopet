#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Creates a Python virtual environment and installs the easyeda2kicad package
# required by lcsc-import.sh.
#
# Usage:
#   tools/lcsc-import/setup.sh

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

echo "Creating virtual environment in $VENV_DIR ..."
python3 -m venv "$VENV_DIR"

# shellcheck source=/dev/null
source "$VENV_DIR/bin/activate"

echo "Installing easyeda2kicad ..."
pip install --upgrade pip
pip install easyeda2kicad

echo ""
echo "Setup complete. lcsc-import.sh will activate this venv automatically."
