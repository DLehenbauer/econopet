// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// IEEE-488 disk-drive emulation (devices 8 and 9, two drives each), backed
// by Commodore disk images on the SD card:
//
//   /disks/drive0..drive3.{d80,d64,hdd} -> slots 0-3 (slot = unit*2 + drive)
//
// The FPGA (ieee.sv) runs the bus handshake and exposes byte FIFOs over
// SPI/Wishbone; this module implements the DOS layer: OPEN by filename,
// the channel-15 status channel per unit, sequential streaming with EOI,
// and CBM relative files (Super-OS/9).
//

// Scans /disks and mounts the conventional images. Leaves the fabric
// transparent (emulation off) until ieee_drive_set_enabled(true).
void ieee_drive_init(void);

// Enables/disables the virtual drives. When off, the fabric is transparent
// so real IEEE-488 drives on the bus work as on a stock PET.
void ieee_drive_set_enabled(bool en);

// Services the fabric FIFOs; call every main-loop pass.
void ieee_drive_task(void);
