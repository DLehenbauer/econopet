#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Clean build of the CMake super-project, followed by the full test suite.
#
# Usage:
#   ./build.sh

set -e

# Save current directory and change to the directory containing this script
ORIG_PWD="$(pwd)"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Ensure we restore the original directory on exit
trap 'cd "$ORIG_PWD"' EXIT

# Clean build directory
rm -rf ./build

# Configure CMake super-project
cmake --preset default

# Building all subprojects
cmake --build --preset all

# Run unit tests
echo "Running unit tests..."
ctest --preset all --parallel --output-on-failure --verbose
