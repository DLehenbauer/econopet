#!/bin/bash
set -e

# Save current directory and change to the directory containing this script
ORIG_PWD="$(pwd)"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Ensure we restore the original directory on exit
trap 'cd "$ORIG_PWD"' EXIT

rm -rf ./build

# Build sdcard
cmake --preset rp2040_release --log-level VERBOSE
cmake --build --preset sdcard

# Build firmware unit tests for Linux host
cmake --preset test --log-level VERBOSE
cmake --build --preset test
#pushd build/test/fw/test && ./firmware-test; popd
