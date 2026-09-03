#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Compile and run a gateware testbench under Verilator (--binary --timing).
# Usage: ./verilate.sh TEST_NAME [RAND_RESET]
#   RAND_RESET: 0 (default; required for m6502), 1 (all ones), or 2 (random).
# See docs/dev/verilator.md.

SCRIPT_DIR="$(readlink -f $(dirname "$0"))"
PROJ_DIR="$SCRIPT_DIR/gw/EconoPET"
TEST_NAME="$1"
RAND_RESET="${2:-0}"

if [ -z "$TEST_NAME" ]; then
    echo "Usage: $0 TEST_NAME [RAND_RESET]"
    exit 1
fi

# Invoke seed.sh to get the effective test seed (empty if none is set).
SEED="$("$SCRIPT_DIR/seed.sh")" || exit 1

# Reuse sim.sh's generated file list.
[ -f "$PROJ_DIR/work_sim/EconoPET.f" ] || "$SCRIPT_DIR/sim.sh" -u >/dev/null

cd "$PROJ_DIR" || exit 1

# Build threads per model; lower it if running many of these at once.
VERILATOR_JOBS="${VERILATOR_JOBS:-$(nproc)}"

verilator --binary --timing -j "$VERILATOR_JOBS" \
    --x-assign unique --x-initial unique \
    -Wno-fatal -Wno-lint -Wno-style "$PROJ_DIR/verilator.vlt" \
    --timescale 1ns/1ps \
    --top-module "$TEST_NAME" \
    --Mdir "work_sim/obj_${TEST_NAME}" -o "${TEST_NAME}_vl" \
    -Iexternal/m6502/rtl \
    -DECONOPET_ROMS_DIR=\"${ECONOPET_ROMS_DIR}\" \
    -f work_sim/EconoPET.f || exit $?

VERILATOR_ARGS=("+verilator+rand+reset+${RAND_RESET}")
if [ -n "$SEED" ]; then
    echo "Test seed: ${SEED} (replay with ECONOPET_TEST_SEED=${SEED})"
    VERILATOR_ARGS+=("+verilator+seed+${SEED}" "+seed=${SEED}")
fi

exec "work_sim/obj_${TEST_NAME}/${TEST_NAME}_vl" "${VERILATOR_ARGS[@]}"
