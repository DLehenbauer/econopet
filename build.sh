#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Incremental build of the CMake super-project, followed by the full test suite.
#
# Usage:
#   ./build.sh [--clean]

set -e

# Validate options before configuring or deleting build output.
CLEAN_BUILD=false
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN_BUILD=true ;;
        *)
            printf 'Unknown argument: %s\nUsage: %s [--clean]\n' "$arg" "$0" >&2
            exit 2
            ;;
    esac
done

# Print a phase heading and record its start time in seconds.
begin_section() {
    SECTION_NAME="$1"
    SECTION_START_SECONDS=$SECONDS
    printf '\n========== %s ==========\n\n' "$SECTION_NAME"
}

# Report elapsed wall time for the completed phase.
end_section() {
    printf '%s completed in %ds\n\n' \
        "$SECTION_NAME" "$((SECONDS - SECTION_START_SECONDS))"
}

# Change to the directory containing this script
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Optionally clean contents without removing the build volume's mount point.
if "$CLEAN_BUILD"; then
    begin_section "Clean"
    mkdir -p ./build
    find ./build -mindepth 1 -delete
    end_section
fi

# Apply current preset settings while retaining incremental build output.
begin_section "Configure"
cmake --preset default
end_section

# Building all subprojects
begin_section "Build"
cmake --build --preset all --verbose
end_section

# Run unit tests
begin_section "Test"
ctest --preset all --parallel --output-on-failure
end_section
