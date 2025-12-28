/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

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
