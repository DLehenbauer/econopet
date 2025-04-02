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

// PET keyboard matrix is 8 rows by 10 columns where each row is represented by a bit in a byte.
// The 'key_matrix' array is indexed by column and contains the row byte for that column.
// A key is pressed when the corresponding bit is 0.
#define KEY_COL_COUNT 10
extern uint8_t key_matrix[KEY_COL_COUNT];
extern uint8_t pet_key_matrix[KEY_COL_COUNT];

//void process_kbd_report(hid_keyboard_report_t const *report);
void dispatch_key_events();