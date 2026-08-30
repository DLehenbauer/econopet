// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// IEEE-488 disk-drive emulation (devices 8 and 9, two drives each), backed
// by Commodore disk images on the SD card:
//
//   /disks/drive0..drive3.{d80,d64} -> slots 0-3 (slot = unit*2 + drive)
//
// The FPGA (ieee.sv) runs the bus handshake and exposes byte FIFOs over
// SPI/Wishbone; this module implements the DOS layer: OPEN by filename,
// the channel-15 status channel per unit, sequential streaming with EOI,
// and CBM relative files (Super-OS/9).
//

void ieee_drive_init(void);

// Enable/disable the virtual IEEE-488 drive (default off = real hardware bus).
// Driven by the config's 'ieee-drive: on|off' option.
void ieee_drive_set_enabled(bool en);

// Services the fabric FIFOs; call every main-loop pass.
void ieee_drive_task(void);

// Formats a compact one-line debug summary (for the HDMI overlay):
// mount state, live FPGA status/SA registers, command/data/stream counters,
// and the last OPEN result.
void ieee_drive_debug_status(char* out, unsigned int len);

// Overlay line for the HDMI renderer: shows a transient "disk swapped"
// toast when recent, otherwise the live debug status.
void ieee_drive_overlay_text(char* out, unsigned int len);

// End-user disk-status line for the on-CRT HUD overlay: "* D0:<img> D1:<img>",
// where the leading marker blinks while a drive is being accessed.
void ieee_drive_hud_status(char* out, unsigned int len);

// Rotate slot n (0-3) to the next image in the library (the mount commits
// once cycling pauses). Shared by the USB hotkeys and PET-keyboard chords.
bool ieee_drive_cycle(unsigned int n);

// Monotonic counter bumped on every image mount/swap. The HUD polls this to
// flash the overlay on a disk change without the mount sites notifying it.
unsigned ieee_drive_mount_generation(void);

// USB hotkey hook (raw HID keycode, before PET-matrix mapping). Returns
// true when consumed: F1-F4 cycle slots 0-3, F8 toggles the status line,
// F11 shows the mounted images.
bool ieee_drive_hotkey(unsigned char hid_keycode);
