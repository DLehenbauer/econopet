#!/bin/bash

set -e

rm -rf ./build

# Build firmware for RP2040
cmake --preset rp2040_release
cmake --build --preset rp2040_release

# Build firmware unit tests for Linux host
cmake --preset test
cmake --build --preset test
#pushd build/test/fw/test && ./firmware-test; popd
