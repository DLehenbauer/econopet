// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "system/state.h"

// Restores the firmware's built-in USB keyboard map into mutable system state.
void usb_keymap_reset(system_state_t* state);
