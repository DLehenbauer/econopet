#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Launch the Efinity STA TCL shell for interactive timing analysis.
#
# Usage:
#   .github/skills/timing-closure/scripts/sta-repl.sh
#
# Prerequisites:
#   - EFINITY_HOME must be set (provided by the dev container).
#   - The FPGA bitstream must have been built at least once so timing data exists.
#
# Once inside the TCL shell, useful commands include:
#
#   reset_timing; delete_timing_results; read_sdc; report_timing_summary
#   report_timing -from_clock sys_clock_i -to_clock sys_clock_i -setup
#   report_timing -from_clock sys_clock_i -to_clock sys_clock_i -hold

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd -- "$SCRIPT_DIR/../../../.." && pwd)"

exec "$WORKSPACE_ROOT/gw/efx.sh" -f sta_tclsh
