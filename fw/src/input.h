// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Extended keycodes for special keys and input sources
// Standard ASCII: 0-127
// EOF: -1

// Navigation keys (terminal escape sequences)
#define KEY_UP           1000
#define KEY_DOWN         1001
#define KEY_RIGHT        1002
#define KEY_LEFT         1003
#define KEY_HOME         1004
#define KEY_END          1005
#define KEY_PGUP         1006
#define KEY_PGDN         1007

// Physical button actions
#define KEY_BTN_SHORT    1100
#define KEY_BTN_LONG     1101

// Initialize input subsystem
void input_init(void);

// Poll all input sources and dispatch based on system_state:
// - Button -> firmware queue
// - USB/PET keyboards -> key matrix (always)
// - UART -> PET keys or firmware queue (based on term_input_dest)
void input_task(void);

// Get next keycode for firmware (menu, etc).
// Returns EOF if no input available.
// Only returns events when term_input_dest == term_input_to_firmware.
int input_getch(void);
