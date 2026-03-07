#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Launch the Efinity STA Tcl shell for non-interactive timing analysis.
#
# Tcl commands are piped via stdin and must end with `exit`:
#
#   echo 'report_timing_summary; exit' \
#       | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
#
# Prerequisites:
#   - EFINITY_HOME must be set (provided by the dev container).
#   - The FPGA bitstream must have been built at least once so timing data exists.

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd -- "$SCRIPT_DIR/../../../.." && pwd)"
PROJECT_DIR="$WORKSPACE_ROOT/gw/EconoPET"
PROJECT_XML="$PROJECT_DIR/EconoPET.xml"

# Efinity 2025.1.110 `sta_tclsh` always defaults to the C4 timing model. To get
# correct results, we extract the project's timing model from EconoPET.xml and
# prepend a `set_timing_model` Tcl command before the caller's stdin.
TIMING_MODEL="$(grep -oP 'timing_model name="\K[^"]+' "$PROJECT_XML")"

# The CMake build places output in build/gw/{outflow,work}, but the STA tool
# expects `outflow/` and `work_pnr/` as siblings of EconoPET.xml. We create a
# staging directory that mirrors the project layout with symlinks, then override
# `outflow` and `work_pnr` to point to the build tree.
BUILD_GW="$WORKSPACE_ROOT/build/gw"
STAGING_DIR="$BUILD_GW/sta-staging"

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

for item in "$PROJECT_DIR"/*; do
	ln -snf "$item" "$STAGING_DIR/$(basename "$item")"
done

ln -snf "$BUILD_GW/outflow" "$STAGING_DIR/outflow"
ln -snf "$BUILD_GW/work" "$STAGING_DIR/work_pnr"

# Source the Efinity environment and run efx_run.py from the staging directory
# (bypassing efx.sh which hardcodes cd to gw/EconoPET/).
source "$EFINITY_HOME/bin/setup.sh"
cd "$STAGING_DIR"

# Prepend `set_timing_model <model>` then replay the caller's original stdin
# (e.g., `echo 'report_timing_summary; exit' | sta-repl.sh`).
exec python3 -u "$EFINITY_HOME/scripts/efx_run.py" EconoPET.xml -f sta_tclsh \
	< <({
		echo "set_timing_model $TIMING_MODEL"
		cat
	})
