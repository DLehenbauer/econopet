#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Run an Efinity flow against gw/EconoPET/EconoPET.xml with the Efinity
# environment sourced.  Arguments are passed through to efx_run.py.
#
# Usage:
#   gw/efx.sh [EFX_RUN_OPTIONS]

set -e

source $EFINITY_HOME/bin/setup.sh

# Ensure we restore the original directory on exit
ORIG_PWD="$(pwd)"
trap 'cd "$ORIG_PWD"' EXIT

# Change to the './EconoPET' directory relative to this script's location
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/EconoPET"

python3 -u $EFINITY_HOME/scripts/efx_run.py EconoPET.xml "$@"
