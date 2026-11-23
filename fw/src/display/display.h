// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "display/window.h"
#include "system/state.h"

/** Standard firmware display geometry. */
#define DISPLAY_COLS_40 40u
#define DISPLAY_COLS_80 80u
#define DISPLAY_ROWS 25u

/** Indices into the DVI renderer's C128 RGBI palette. */
typedef enum {
    DISPLAY_COLOR_BLACK, DISPLAY_COLOR_MEDIUM_GRAY, DISPLAY_COLOR_BLUE, DISPLAY_COLOR_LIGHT_BLUE,
    DISPLAY_COLOR_GREEN, DISPLAY_COLOR_LIGHT_GREEN, DISPLAY_COLOR_DARK_CYAN, DISPLAY_COLOR_LIGHT_CYAN,
    DISPLAY_COLOR_RED, DISPLAY_COLOR_LIGHT_RED, DISPLAY_COLOR_DARK_MAGENTA, DISPLAY_COLOR_MAGENTA,
    DISPLAY_COLOR_DARK_YELLOW, DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_LIGHT_GRAY, DISPLAY_COLOR_WHITE,
    DISPLAY_COLOR_COUNT,
} display_color_t;

/** Pack foreground and background RGBI palette indices. */
#define DISPLAY_ATTR(fg, bg) ((uint8_t)(((uint8_t)(bg) << 4) | ((uint8_t)(fg) & 0x0fu)))

/** PET address corresponding to the start of the shared video buffer. */
#define VIDEO_RAM_ADDRESS 0x8000u
/** Size in bytes of each character or color span in 40-column ColourPET mode. */
#define VIDEO_RAM_UNIT_BYTES 0x400u
/** Maximum character or color plane size in bytes for 40- and 80-column rendering. */
#define VIDEO_PLANE_CAPACITY 0x800u
/** Byte offset of color attributes within the shared video buffer. */
#define VIDEO_COLOR_OFFSET VIDEO_PLANE_CAPACITY
/** Color attribute capacity in bytes, including 80-column rendering. */
#define VIDEO_COLOR_CAPACITY VIDEO_PLANE_CAPACITY
/** PET address of the color plane when color RAM is enabled. */
#define VIDEO_COLOR_ADDRESS (VIDEO_RAM_ADDRESS + VIDEO_COLOR_OFFSET)
/** White foreground (low nibble) on black background (high nibble). */
#define VIDEO_DEFAULT_ATTRIBUTE DISPLAY_ATTR(DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK)

/** Bind a color window to the active firmware display. */
window_t display_window(void);

/** Initialize local display state without FPGA access. */
window_t display_init_local(pet_display_columns_t columns, uint8_t attr);

/** Initialize local and FPGA video state for a firmware-owned display. */
window_t display_init_firmware(pet_display_columns_t columns, uint8_t attr);

/** Synchronize the active video planes and terminal mirror. */
void display_flush(void);

// Initialize display subsystem
void display_init(void);

// Sync video between PET RAM and the DVI display.  The transfer direction is
// controlled by system_state.video_source.  Render to the terminal if it is
// mirroring PET video.
void display_task(void);

/** Report whether the video buffer belongs on the terminal. */
bool display_term_video_active(void);

/** Clear and upload the full character plane. The caller must halt the PET CPU. */
void display_clear(void);

// Terminal ANSI rendering control (for menu mode)
void display_term_begin(void);   // Enter alternate screen, hide cursor
void display_term_end(void);     // Exit alternate screen, show cursor
void display_term_refresh(void); // Immediately render video buffer to terminal
