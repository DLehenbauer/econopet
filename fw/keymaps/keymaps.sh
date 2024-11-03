#!/bin/bash
for r in *.bin
do
    f="$(basename -- $r .bin).h"
    cat $r | xxd -i > $f
done
