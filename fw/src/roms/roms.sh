#!/bin/bash
for r in ../../../build/roms/bin/roms/*
do
    f="$(basename -- $r .bin).h"
    cat $r | xxd -i > $f
done

# Create a modified edit ROM for 80 cols / Graphics keyboard
cp edit-4-80-b-60Hz.901474-03.h edit-4-80-n-60Hz.901474-03-hack.h
patch edit-4-80-n-60Hz.901474-03-hack.h edit-4-80-n-60Hz.901474-03-hack.diff

cat ../../../rom/rom.bin | xxd -i > menu.h
