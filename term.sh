#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Open a serial console on the RP2040's USB CDC port at 115200 baud.
#
# Usage:
#   ./term.sh

# Check if /dev/ttyACM0 exists
if [ ! -e /dev/ttyACM0 ]; then
    echo "Error: /dev/ttyACM0 not found."
    echo "Please run 'sudo modprobe cdc_acm' on your Ubuntu WSL distro."
    exit 1
fi

minicom -b 115200 -o -D /dev/ttyACM0
