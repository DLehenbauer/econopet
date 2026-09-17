#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Create the local Python environment and run the TPS564242 divider calculator.
# Usage: ./run.sh --vout VOLTS [--vmin VOLTS] [--vmax VOLTS] [--max-parallel COUNT]

set -euo pipefail

SCRIPT_DIR="$(readlink -f "$(dirname "$0")")"
VENV="$SCRIPT_DIR/.venv"

if [[ ! -x "$VENV/bin/python" ]]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -r "$SCRIPT_DIR/requirements.txt"
fi

exec "$VENV/bin/python" "$SCRIPT_DIR/main.py" "$@"