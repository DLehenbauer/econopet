// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

#include "ieee/dos_media.h"

// IEEE-488 disk-drive emulation (devices 8 through 11, two drives each),
// backed by Commodore disk images in /disk on the SD card. Slots 0-7 map to
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

/** Return true when any virtual IEEE-488 unit (devices 8 through 11) has mounted media. */
bool ieee_drive_enabled(void);

// Borrows a slot's canonical disk/-relative name, or "" if empty. A successful mount,
// unmount, or abort can invalidate the contents. Rejected mounts retain the name.
const char* ieee_drive_filename(unsigned int slot);

// Successful host-mount effects for callers such as the Change Disk menu.
typedef struct {
    int moved_from_slot; // -1 when no image moved from another slot.
    diskimage_type_t ejected_partner;
} ieee_drive_mount_outcome_t;

// Mounts a path relative to /disk into a slot (0-7), or removes it for NULL.
// A successful mount moves an already-mounted image and ejects an incompatible
// destination partner. Failure retains all bindings in the affected units.
// Busy transport or pending work returns DOS_MEDIA_BUSY.
// Busy rejection does not finalize units or discard their staged output.
// Moves and same-slot remounts reuse the existing storage handle.
// Names are normalized and must remain inside /disk.
// If non-NULL, outcome receives successful effects and is unchanged on failure.
// A full /disk/ path exceeding SD_PATH_MAX (sd/sd.h), including the terminating null, is fatal.
dos_media_result_t ieee_drive_try_mount(unsigned int slot, const char* filename, ieee_drive_mount_outcome_t* outcome);

// Services the fabric FIFOs; call every main-loop pass.
void ieee_drive_task(void);
