#!/bin/bash
# Compile and run a gateware testbench under Verilator (--binary --timing).
# Usage: ./verilate.sh TEST_NAME [RAND_RESET]
#   RAND_RESET: 0 (default; required for m6502 benches) or 1 (mc6809 benches).
# See docs/dev/verilator.md.

SCRIPT_DIR="$(readlink -f $(dirname "$0"))"
PROJ_DIR="$SCRIPT_DIR/gw/EconoPET"
TEST_NAME="$1"
RAND_RESET="${2:-0}"

if [ -z "$TEST_NAME" ]; then
    echo "Usage: $0 TEST_NAME [RAND_RESET]"
    exit 1
fi

# Reuse sim.sh's generated file list.
[ -f "$PROJ_DIR/work_sim/EconoPET.f" ] || "$SCRIPT_DIR/sim.sh" -u >/dev/null

cd "$PROJ_DIR" || exit 1

verilator --binary --timing -j "$(nproc)" \
    --x-assign unique --x-initial unique \
    -Wno-fatal -Wno-lint -Wno-style \
    --timescale 1ns/1ps \
    --top-module "$TEST_NAME" \
    --Mdir "work_sim/obj_${TEST_NAME}" -o "${TEST_NAME}_vl" \
    -Iexternal/m6502/rtl \
    -DECONOPET_ROMS_DIR=\"${ECONOPET_ROMS_DIR}\" \
    -f work_sim/EconoPET.f || exit $?

exec "work_sim/obj_${TEST_NAME}/${TEST_NAME}_vl" "+verilator+rand+reset+${RAND_RESET}"
