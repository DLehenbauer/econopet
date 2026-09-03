#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Compile and run gw/EconoPET/sim/scratch.sv under Icarus Verilog.  A scratch
# bench for quick experiments, outside the CTest suite that sim.sh drives.
#
# Usage:
#   ./scratch.sh

PROJNAME="EconoPET"
PROJDIR="$(readlink -f $(dirname "$0"))/gw/$PROJNAME"

pushd "$PROJDIR" || exit 1

iverilog -g2009 -s "sim" -o"/tmp/scratch.vvp" -f"$PROJDIR/work_sim/timescale.f" ./sim/scratch.sv
if [ $? -ne 0 ]; then
    popd && exit $?
fi

vvp -l"$PROJDIR/outflow/scratch.rtl.simlog" "/tmp/scratch.vvp"
popd && exit $?
