#!/bin/bash
for r in ../../build/roms/bin/roms/*
do
    f="$(basename -- $r .bin).h"
    cat $r | xxd -i > $f
done