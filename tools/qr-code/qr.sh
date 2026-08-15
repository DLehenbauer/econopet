#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <URL> <IMAGE_FILE>"
    echo "Example: $0 https://www.example.com Foo.png"
    exit 1
fi

# Activate the virtual environment (run setup.sh first if it does not exist)
if [[ ! -d "$VENV_DIR" ]]; then
    echo "Virtual environment not found. Running setup.sh ..."
    bash "$SCRIPT_DIR/setup.sh"
fi

source "$VENV_DIR/bin/activate"

qr --output "$2" "$1"
