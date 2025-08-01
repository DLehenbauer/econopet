#!/bin/bash

# Check if /dev/ttyACM0 exists
if [ ! -e /dev/ttyACM0 ]; then
    echo "Error: /dev/ttyACM0 not found."
    echo "Please run 'sudo modprobe cdc_acm' on your Ubuntu WSL distro."
    exit 1
fi

minicom -b 115200 -o -D /dev/ttyACM0
