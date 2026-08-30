// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

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
#define PET_KEY_T_N         (PET_KEY_EVENT(true, 2, 2))

#define PET_KEY_DOWN_B      (PET_KEY_EVENT(true, 4, 5))
#define PET_KEY_RIGHT_B     (PET_KEY_EVENT(true, 5, 0))
#define PET_KEY_RETURN_B    (PET_KEY_EVENT(true, 4, 3))
#define PET_KEY_LSHIFT_B    (PET_KEY_EVENT(true, 6, 6))
#define PET_KEY_RSHIFT_B    (PET_KEY_EVENT(true, 0, 6))
#define PET_KEY_T_B         (PET_KEY_EVENT(true, 2, 5))

#define PET_KEY_STOP        (PET_KEY_EVENT(true, 4, 9))
#define PET_KEY_RVS_N       (PET_KEY_EVENT(true, 0, 9))
#define PET_KEY_RVS_B       (PET_KEY_EVENT(true, 0, 8))

// Number keys '0' and '1' (model-specific cells; graphics = numeric keypad,
// business = top-row). Used with SHIFT to cycle drive 0-3 images.
#define PET_KEY_0_N         (PET_KEY_EVENT(true, 6, 8))
#define PET_KEY_0_B         (PET_KEY_EVENT(true, 3, 1))
#define PET_KEY_1_N         (PET_KEY_EVENT(true, 6, 6))
#define PET_KEY_1_B         (PET_KEY_EVENT(true, 0, 1))
#define PET_KEY_2_N         (PET_KEY_EVENT(true, 6, 7))
#define PET_KEY_2_B         (PET_KEY_EVENT(true, 0, 0))
#define PET_KEY_3_N         (PET_KEY_EVENT(true, 7, 6))
#define PET_KEY_3_B         (PET_KEY_EVENT(true, 1, 9))
#define PET_KEY_0K_B        (PET_KEY_EVENT(true, 4, 7))
#define PET_KEY_1K_B        (PET_KEY_EVENT(true, 7, 8))
#define PET_KEY_2K_B        (PET_KEY_EVENT(true, 7, 7))
#define PET_KEY_3K_B        (PET_KEY_EVENT(true, 7, 6))

// Sentinel for "no key event". (Note that column '0xF' exceeds KEY_COL_COUNT)
extern const key_event_t key_event_none;

key_event_t next_key_event(const uint8_t* matrix);
int keyscan_getch(const uint8_t matrix[10]);

// True if the key identified by 'event' (row/col packed via PET_KEY_EVENT) is
// currently held down in 'matrix' (a pressed key clears its bit).
bool is_key_down(const uint8_t matrix[10], key_event_t event);
