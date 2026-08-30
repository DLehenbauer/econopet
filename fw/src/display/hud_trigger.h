// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Real-PET-keyboard trigger + content policy for the on-CRT HUD overlay.
//
// Watches the snooped PET key matrix for a held SHIFT to pin the
// overlay on/off, and auto-flashes it for a few seconds after any disk swap.
// While shown, it repaints the disk-status line (live image names + a
// blinking access marker).
//
// Call once per main-loop pass, immediately after input_task() (which refreshes
// pet_key_matrix via sync_state()).
void hud_trigger_task(void);

// Pin the overlay on/off (used by the SHIFT-hold trigger and the USB hotkey).
void hud_trigger_toggle(void);

// USB hotkey hook (raw HID keycode): F7 toggles the overlay. Returns true when
// the key was consumed. Called from the USB keyboard layer.
bool hud_trigger_hotkey(uint8_t hid_keycode);
