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

// Capacity for ordinary formatted DOS status lines, not arbitrary channel-15
// data such as memory-read replies. DOS_4040 and DOS_8250 ramvar place ERRBUF
// in the final 36 bytes before $4400. Reserve that text, its transmitted CR,
// and a host C terminating null (which is not transmitted).
// Sources: https://github.com/mist64/cbmsrc/blob/master/DOS_4040/ramvar
//          https://github.com/mist64/cbmsrc/blob/master/DOS_8250/ramvar
#define IEEE_STATUS_TEXT_MAX 36u
#define IEEE_STATUS_LINE_CAPACITY (IEEE_STATUS_TEXT_MAX + 1u + 1u)

// Initializes IEEE-488 emulation with no mounted images and fresh DOS/channel
// state. The fabric remains transparent until an image is mounted.
void ieee_drive_init(void);

// Resets DOS and channel state and flushes the fabric FIFOs for a
// firmware-initiated PET reset. Mounted images remain attached.
void ieee_drive_reset(void);

// Removes all mounted disk images.
void ieee_drive_unmount_all(void);

// Mounts a path relative to /disks into a slot (0-7). Returns false when the
// file was not found or is not a supported disk image. A full /disks/ path
// exceeding SD_PATH_MAX (sd/sd.h), including the terminating null, is fatal.
bool ieee_drive_mount(unsigned int slot, const char* filename);

// Services the fabric FIFOs; call every main-loop pass.
void ieee_drive_task(void);
