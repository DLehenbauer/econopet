// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Firmware DOS layer for the fabric's IEEE-488 drive emulation (see
// gw/EconoPET/src/ieee.sv): units 8 and 9, two drives each, served from
// d80/d64 images in /disks on the SD card. Mount slot = unit * 2 + drive;
// drive0..drive3.{d80,d64} are mounted by name at init.

// Scans /disks and mounts the conventional images. Leaves the fabric
// transparent (emulation off) until ieee_drive_set_enabled(true).
void ieee_drive_init(void);

// Enables/disables the virtual drives. When off, the fabric is transparent
// so real IEEE-488 drives on the bus work as on a stock PET.
void ieee_drive_set_enabled(bool en);

// Services the fabric FIFOs; call every main-loop pass.
void ieee_drive_task(void);
