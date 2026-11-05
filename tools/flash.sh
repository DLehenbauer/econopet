#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Flash and verify firmware, retrying once with serialized CMSIS-DAP transfers.
# Per section `A.6.2. Debug with SWD`:
# https://pip-assets.raspberrypi.com/categories/609-microcontroller-boards/documents/RP-008276-DS-4-getting-started-with-pico.pdf

set -e

# Resolve the firmware path from the repository root, regardless of the caller.
cd -- "$(dirname -- "$0")/.."

openocd_args=(
  -f interface/cmsis-dap.cfg
  -f target/rp2040.cfg
)
program_command="program build/fw/src/firmware.elf verify reset exit"

# Keep the normal pipelined transfer path when it succeeds.
# Reduce the adapter speed if 'openocd' reports the following error:
# 
#    Error: Failed to connect multidrop rp2040.dap0
#
# Documentation uses 5000 kHz:
# https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf
#
# Latest Debugprobe firmware available here:
# https://github.com/raspberrypi/debugprobe
#
# Notes:
#  - 31250 kHz fails immediately with Debugprobe v2.3.1.
#  - Empirically, 20833 KHz is more reliable than 31249 KHz when pipelined.
if openocd "${openocd_args[@]}" -c "adapter speed 20833" -c "init" -c "$program_command"; then
  exit 0
fi

# `cmsis-dap quirk enable` serializes CMSIS-DAP transfers to work around
# intermittent probe communication failures that can report "Could not allocate
# bounce buffer for flash programming". Quirk mode requires initialization
# before serializing CMSIS-DAP transfers.
if openocd "${openocd_args[@]}" \
  -c "adapter speed 5000" \
  -c "init" \
  -c "cmsis-dap quirk enable" \
  -c "$program_command"; then
  printf '\033[33m%s\033[0m\n' "Warning: Normal flash failed, but recovery with serialized CMSIS-DAP transfers succeeded." >&2
else
  status=$?
  printf '\033[31m%s\033[0m\n' "Error: Normal flash and recovery with serialized CMSIS-DAP transfers both failed." >&2
  exit "$status"
fi
