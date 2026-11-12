// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "system_state.h"

void video_init();

/** Stop core 1 consuming shared display data at a blank boundary, with a bounded wait. */
bool video_transition_begin(void);
/** Publish stable display state and wait for core 1 to resume consumption. */
bool video_transition_end(void);
/** Select contrasting menu attributes without modifying guest color storage. */
void video_set_menu_attributes(bool enabled);
