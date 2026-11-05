#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Run a prebuilt Verilator testbench with the shared test seed.
# Usage: ./verilate.sh TEST_NAME_OR_EXECUTABLE [RAND_RESET]
#   RAND_RESET: 0 (default; required for m6502), 1 (all ones), or 2 (random).
# See docs/dev/verilator.md.

set -euo pipefail

SCRIPT_DIR="$(readlink -f "$(dirname "$0")")"
MODEL="${1:-}"
RAND_RESET="${2:-0}"

if [ -z "$MODEL" ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 TEST_NAME_OR_EXECUTABLE [RAND_RESET]" >&2
    exit 1
fi

# CTest supplies the exact target path. Direct invocations use the default preset.
if [[ "$MODEL" != */* ]]; then
    MODEL="$SCRIPT_DIR/build/gw-test/bin/${MODEL}_vl"
fi
if [ ! -x "$MODEL" ]; then
    echo "Error: $MODEL is missing. Run cmake --build --preset gw-test first." >&2
    exit 1
fi
MODEL="$(readlink -f "$MODEL")"
SEED="$("$SCRIPT_DIR/seed.sh")"

VERILATOR_ARGS=("+verilator+rand+reset+${RAND_RESET}")
if [ -n "$SEED" ]; then
    echo "Test seed: ${SEED} (replay with ECONOPET_TEST_SEED=${SEED})"
    VERILATOR_ARGS+=("+verilator+seed+${SEED}" "+seed=${SEED}")
fi

# SID lookup tables and testbench output paths are project-relative.
cd "$SCRIPT_DIR/gw/EconoPET"
mkdir -p outflow
exec "$MODEL" "${VERILATOR_ARGS[@]}"
