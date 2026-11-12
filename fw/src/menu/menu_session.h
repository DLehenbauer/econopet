// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MENU_SCREEN_ADDRESS 0x8000u
#define MENU_SCREEN_BYTES 0x400u
#define MENU_FONT_BYTES 0x800u

/** Begin one runtime borrowing session without changing the selected CPU or resetting it. */
bool menu_session_begin(void);
/** Restore borrowed state, retaining suspension for reset/configuration when resume is false. */
void menu_session_restore(bool resume);
/** Retire any context and hand off to reset/diagnostics without an old-context execution window. */
void menu_session_abandon(void);
/** Report lifecycle ownership, including partial entry and restoration. */
bool menu_session_active(void);
