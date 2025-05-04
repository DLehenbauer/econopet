#!/bin/bash
ROM_ROOT="../../../rom"

pushd $ROM_ROOT

# Ensure build directory is empty
[ -d "build" ] && rm -rf "build"
mkdir -p "build"

cd ./build
cmake ..
cmake --build .
popd

cat $ROM_ROOT/build/bin/menu.rom | xxd -i > menu_rom.h
