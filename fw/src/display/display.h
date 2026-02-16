// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "display/window.h"

// Initialize display subsystem
void display_init(void);

// Sync video between PET RAM and HDMI buffer based on system_state.video_source
// Optionally render to terminal based on system_state.term_mode
void display_task(void);

// Terminal ANSI rendering control (for menu mode)
void display_term_begin(void);   // Enter alternate screen, hide cursor
void display_term_end(void);     // Exit alternate screen, show cursor
void display_term_refresh(void); // Immediately render video buffer to terminal

// Window-based terminal display (for fatal/config error display)
void display_window_begin(const window_t* window);   // Enter alternate screen, fill window
void display_window_show(const window_t* window);    // Render window to terminal
