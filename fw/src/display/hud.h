// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Firmware side of the FPGA HUD overlay device (see gw/EconoPET/src/hud.sv).
//
// Firmware hands the fabric a run of PET screen codes; the fabric
// substitutes them over the video fetch at scan-out, so the overlay appears
// on the CRT without writing video RAM. The fabric composites on the CRT
// only -- call hud_paint_hdmi() from the HDMI path to keep both in sync.

// Largest overlay run we support (one 80-column row). The fabric buffer is
// 256 bytes; a status line never approaches that.
#define HUD_MAX_LEN 80

void hud_init(void);

// True while the overlay is being shown.
bool hud_is_active(void);

// Show 'len' pre-encoded PET screen codes (bit7 = reverse video) at VRAM byte
// offset 'off'. Safe to call every frame with fresh content.
void hud_show_codes(uint16_t off, const uint8_t* codes, size_t len);

// Convenience: encode an ASCII string to reverse-video screen codes and show it
// at (row, col) using the active display width. Returns the number of columns
// written (clamped to HUD_MAX_LEN and the row width).
size_t hud_show_text(uint8_t row, uint8_t col, const char* s);

// Hide the overlay (disable the fabric substitution).
void hud_hide(void);

// Paint the current overlay into the firmware's HDMI video buffer (which
// mirrors VRAM linearly). The fabric composites the overlay on the CRT only;
// call this from the HDMI/display path so both surfaces match. No-op when the
// overlay is inactive.
void hud_paint_hdmi(uint8_t* buf, size_t buf_bytes);
