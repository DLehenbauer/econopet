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

#include "../pch.h"
#include "system_state.h"

// PET keyboard matrix is 8 rows by 10 columns where each row is represented by a bit in a byte.
// The 'key_matrix' array is indexed by column and contains the row byte for that column.
// A key is pressed when the corresponding bit is 0.
#define KEY_COL_COUNT 10
#define KEY_ROW_COUNT 8
extern uint8_t usb_key_matrix[KEY_COL_COUNT];
extern uint8_t pet_key_matrix[KEY_COL_COUNT];

uint8_t key_rc_to_i(uint8_t col, uint8_t row);
int keyboard_getch();

// Enqueue synthesized key events (used by terminal injection)
void usb_keyboard_enqueue_key_down(uint8_t keycode, uint8_t modifiers);
void usb_keyboard_enqueue_key_up(uint8_t keycode, uint8_t modifiers);

void usb_keyboard_reset(system_state_t* system_state);
void usb_keyboard_added(uint8_t dev_addr, uint8_t instance);
void usb_keyboard_removed(uint8_t dev_addr, uint8_t instance);
void process_kbd_report(uint8_t dev_addr, hid_keyboard_report_t const* report);
void dispatch_key_events();
