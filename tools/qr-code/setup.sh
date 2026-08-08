#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Creates a Python virtual environment and installs the qrcode package
# required by qr-code.sh.
#
# Usage:
#   tools/qr-code/setup.sh

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv".

echo "Creating virtual environment in $VENV_DIR ..."
python3 -m venv "$VENV_DIR"

# shellcheck source=/dev/null
source "$VENV_DIR/bin/activate"

echo "Installing/updating qrcode ..."
pip install --upgrade pip
pip install --upgrade qrcode

echo ""
echo "Setup complete. qr-code.sh will activate this venv automatically."
