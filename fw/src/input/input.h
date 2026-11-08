// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include "input/input_actions.h"

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

// Initialize input subsystem
void input_init(void);

// Poll all input sources and dispatch based on system_state:
// - Button -> debounced control state
// - USB -> source-owned PET keys or decoded menu presses
// - Physical PET snapshots -> decoded menu presses (runtime typing is hardware-owned)
// - UART -> CLI or shared remote routing according to the selected terminal mode
void input_task(void);
