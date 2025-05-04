#!/bin/bash
REPO_ROOT=$(git rev-parse --show-toplevel)
if [ -z "$REPO_ROOT" ]; then
    echo "Error: Unable to determine the repository root."
    exit 1
fi
ROM_ROOT="$REPO_ROOT/rom"

pushd $ROM_ROOT

# Ensure build directory is empty
[ -d "build" ] && rm -rf "build"
mkdir -p "build"

cd ./build
cmake ..
cmake --build .
popd

cat $REPO_ROOT/build/sdcard/sdcard_root/roms/901447-10.bin | xxd -i > 901447_10.h
cat $ROM_ROOT/build/bin/menu.rom | xxd -i > menu_rom.h
