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

#include "pch.h"

typedef uint8_t key_event_t;

#define PET_KEY_EVENT(pressed, row, col) \
    ((key_event_t)((pressed ? 0x80 : 0x00) | row << 4 | col))

#define PET_KEY_ROW(key_event) \
    (((key_event) >> 4) & 0x07)

#define PET_KEY_COL(key_event) \
    ((key_event) & 0x0F)

#define PET_KEY_IS_PRESSED(key_event) \
    (((key_event) & 0x80) != 0)

#define PET_KEY_DOWN_N      (PET_KEY_EVENT(true, 6, 1))
#define PET_KEY_RIGHT_N     (PET_KEY_EVENT(true, 7, 0))
#define PET_KEY_RETURN_N    (PET_KEY_EVENT(true, 5, 6))
#define PET_KEY_LSHIFT_N    (PET_KEY_EVENT(true, 0, 8))
#define PET_KEY_RSHIFT_N    (PET_KEY_EVENT(true, 5, 8))

#define PET_KEY_DOWN_B      (PET_KEY_EVENT(true, 4, 5))
#define PET_KEY_RIGHT_B     (PET_KEY_EVENT(true, 5, 0))
#define PET_KEY_RETURN_B    (PET_KEY_EVENT(true, 4, 3))
#define PET_KEY_LSHIFT_B    (PET_KEY_EVENT(true, 6, 6))
#define PET_KEY_RSHIFT_B    (PET_KEY_EVENT(true, 0, 6))

// Sentinel for "no key event". (Note that column '0xF' exceeds KEY_COL_COUNT)
extern const key_event_t key_event_none;

key_event_t next_key_event(const uint8_t* matrix);
int keyscan_getch(const uint8_t matrix[10]);
