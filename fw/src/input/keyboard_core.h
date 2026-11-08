// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input_actions.h"
#include "system_state.h"

// Active-low PET-format matrices, indexed by column with one bit per row.
#define KEY_COL_COUNT 10
#define KEY_ROW_COUNT 8
extern uint8_t usb_key_matrix[KEY_COL_COUNT];
extern uint8_t pet_key_matrix[KEY_COL_COUNT];

/** Apply modifier ownership, releases, presses, then the final modifier update as one batch. */
void keyboard_core_submit(const keyboard_batch_t* batch);
/** Release one producer without changing another producer's held keys or modifiers. */
void keyboard_core_cancel(input_source_t source);
/** Retire all runtime ownership and queued transitions at a destination boundary. */
void keyboard_core_cancel_all(void);
/** Restore default mapping and lock settings after the router cancels input. */
void keyboard_core_reset(system_state_t* state);
/** Dispatch one modifier-consistent group, returning whether lock settings changed. */
bool keyboard_core_dispatch(void);
/** Report Caps Lock for the USB adapter's LED output. */
bool keyboard_core_caps_lock(void);
/** Report the effective model for the USB adapter's LED output. */
pet_keyboard_model_t keyboard_core_model(void);
/** Report the effective mapping kind for the USB adapter's LED output. */
usb_keymap_kind_t keyboard_core_keymap_kind(void);
