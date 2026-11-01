// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// IEEE-488 disk-drive emulation (devices 8 through 11, two drives each),
// backed by Commodore disk images in /disks on the SD card. Slots 0-7 map to
// (device - 8) * 2 + drive.
//
// The FPGA (ieee.sv) runs the bus handshake and exposes byte FIFOs over
// SPI/Wishbone. The facade routes requests to the ROM-structured DOS engine,
// including ordinary and REL reads/writes, commands, CLOSE and channel-15
// status. Unsupported operations explicitly fail. There is no legacy fallback.

// Initializes IEEE-488 emulation with no mounted images and fresh DOS/channel
// state. The fabric remains transparent until an image is mounted.
void ieee_drive_init(void);

// Resets DOS and channel state and flushes the fabric FIFOs for a
// firmware-initiated PET reset. Mounted images remain attached.
//
// NOTE: PET/fabric resets *must* go through the MCU by calling `pet_reset()`,
//       which will include a call to `ieee_drive_reset()`.
//
//       Pulling the CPU /RESB pin low independently is unsupported, as
//       firmware does not detect or recover from an independent fabric reset.
void ieee_drive_reset(void);

// Removes all mounted images. Failure retains native media for a later retry.
bool ieee_drive_unmount_all(void);

#if !defined(PICO_ON_DEVICE) || !PICO_ON_DEVICE
// Discards dirty simulator state and closes storage without SPI after a failed
// board is permanently retired. Never substitute this for durable unmount.
void ieee_drive_abort(void);
#endif

// Mounts a path relative to /disks into a slot (0-7), or removes it for NULL.
// Returns false for unavailable/unsupported media, a type mismatch with the
// other slot, or a busy bus, retaining the old unit. A full /disks/ path
// exceeding SD_PATH_MAX (sd/sd.h), including the terminating null, is fatal.
bool ieee_drive_mount(unsigned int slot, const char* filename);

// Services the fabric FIFOs; call every main-loop pass.
void ieee_drive_task(void);
