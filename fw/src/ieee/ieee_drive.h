// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// IEEE-488 disk-drive emulation (devices 8 through 11, two drives each),
// backed by Commodore disk images in /disks on the SD card. Slots 0-7 map to
// (device - 8) * 2 + drive.
//
// The FPGA (ieee.sv) runs the bus handshake and exposes byte FIFOs over
// SPI/Wishbone; this module implements the DOS layer: OPEN by filename,
// the channel-15 status channel per unit, sequential streaming with EOI,
// and CBM relative files (Super-OS/9).
//

// Scans /disks. The fabric remains transparent until an image is mounted.
void ieee_drive_init(void);

// Removes all mounted disk images.
void ieee_drive_unmount_all(void);

// Mounts a path relative to /disks into a slot (0-7). Returns false when the
// file was not found or is not a supported disk image.
bool ieee_drive_mount(unsigned int drive, const char* filename);

// Services the fabric FIFOs; call every main-loop pass.
void ieee_drive_task(void);
