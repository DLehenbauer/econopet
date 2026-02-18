#!/bin/bash
set -e

source $EFINITY_HOME/bin/setup.sh

# Ensure we restore the original directory on exit
ORIG_PWD="$(pwd)"
trap 'cd "$ORIG_PWD"' EXIT

# Change to the './EconoPET' directory relative to this script's location
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/EconoPET"

python3 -u $EFINITY_HOME/scripts/efx_run.py EconoPET.xml "$@"
