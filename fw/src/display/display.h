// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "display/window.h"

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
#define VIDEO_DEFAULT_ATTRIBUTE 0x0fu

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

// Window-based terminal display (for fatal/config error display)
void display_window_begin(const window_t* window);   // Enter alternate screen, fill window
void display_window_show(const window_t* window);    // Render window to terminal
