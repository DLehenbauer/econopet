#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv."

# Resolve to an absolute, canonical path (no '..') so easyeda2kicad's
# Path.relative_to(Path.cwd()) check in --project-relative mode succeeds.

if [[ $# -eq 0 ]]; then
    echo "Usage: $0 <URL>"
    echo "Example: $0 https://www.example.com"
    exit 1
fi

# Activate the virtual environment (run setup.sh first if it does not exist)
if [[ ! -d "$VENV_DIR" ]]; then
    echo "Virtual environment not found. Running setup.sh ..."
    bash "$SCRIPT_DIR/setup.sh"
fi

source "$VENV_DIR/bin/activate"

qr "$1"
