// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "blockdevice/blockdevice.h"

// Required physical SD inactivity before a runtime reset or menu entry.
#define SD_ACTIVITY_QUIET_US 500000u

// Instrument the single SD device before it is mounted.
void sd_activity_install(blockdevice_t* device);

// Reports whether SD_ACTIVITY_QUIET_US has elapsed since the last read/program.
// The device is initially quiet, including at uptime zero.
bool sd_activity_quiet(void);
