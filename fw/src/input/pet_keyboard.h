// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Acquire the runtime matrix unless direct scanning owns it. */
void pet_keyboard_sync(void);
/** Begin direct menu scanning after PIA1 is configured, retaining unrelated ORA bits. */
void pet_keyboard_begin(uint8_t saved_ora);
/** Configure ordinary PET keyboard directions when no guest exists at boot. */
void pet_keyboard_begin_boot(void);
/** Complete one bounded scanner step without blocking other cooperative service. */
void pet_keyboard_task(void);
/** Stop issuing physical accesses before PIA1 restoration. */
void pet_keyboard_end(void);
/** Report whether direct scanning owns the physical matrix. */
bool pet_keyboard_active(void);
/** Report whether a full matrix has been published since entry. */
bool pet_keyboard_ready(void);
/** Require a fresh, stable complete matrix before accepting physical key release. */
void pet_keyboard_wait_release_begin(void);
/** Check the completed physical matrix for the exit release guard. */
bool pet_keyboard_released(void);
