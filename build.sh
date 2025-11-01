#!/bin/bash
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
cmake -S . -B build -G Ninja

# Building all subprojects
cmake --build build

# Run unit tests
echo "Running unit tests..."
ctest --test-dir build --output-on-failure --verbose
