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

#include "pch.h"
#include "keyscan.h"

#include "input.h"
#include "keyboard.h"

// Sentinel for "no key event". (Note that column '0xF' exceeds KEY_COL_COUNT)
const key_event_t key_event_none = PET_KEY_EVENT(false, 0x7, 0xF);

static void next_rc(unsigned int* row, unsigned int* col) {
    unsigned int r = *row;
    unsigned int c = *col;

    r = (r + 1) % 8;
    if (r == 0) {
        c = (c + 1) % KEY_COL_COUNT;
    }

    *row = r;
    *col = c;
}

// This function scans the given PET keyboard matrix and returns the next PET key event
// or key_event_none if no key is pressed.
key_event_t next_key_event(const uint8_t* matrix) {
    static uint8_t previous[KEY_COL_COUNT] = {
        0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff
    };

    static unsigned int r = 0;
    static unsigned int c = 0;

    unsigned int start_r = r;
    unsigned int start_c = c;

    key_event_t event = key_event_none;

    do {
        assert(r < 8);
        assert(c < KEY_COL_COUNT);

        // Check if the bit is set in the current (row, column) position.
        const uint8_t mask = 1 << r;

        if ((matrix[c] & mask) == 0 && (previous[c] & mask) != 0) {
            // Key is pressed and was previously released
            event = PET_KEY_EVENT(/* pressed: */ true, r, c);
            previous[c] &= ~mask;
        } else if ((matrix[c] & mask) != 0 && (previous[c] & mask) == 0) {
            // Key is released and was previously pressed
            event = PET_KEY_EVENT(/* pressed: */ false, r, c);
            previous[c] |= mask;
        }
        
        next_rc(&r, &c);
    } while (event == key_event_none && (r != start_r || c != start_c));

    return event;
}

bool is_key_down(const uint8_t matrix[KEY_COL_COUNT], key_event_t event) {
    assert(event != key_event_none);
 
    const unsigned int row = PET_KEY_ROW(event);
    const unsigned int col = PET_KEY_COL(event);
    const uint8_t mask = 1 << row;
    
    return (matrix[col] & mask) == 0;
}

int keyscan_is_shifted(const uint8_t matrix[KEY_COL_COUNT]) {
    return is_key_down(matrix, PET_KEY_LSHIFT_N) ||
           is_key_down(matrix, PET_KEY_LSHIFT_B) ||
           is_key_down(matrix, PET_KEY_RSHIFT_N) ||
           is_key_down(matrix, PET_KEY_RSHIFT_B);
}

int keyscan_getch(const uint8_t matrix[KEY_COL_COUNT]) {
    static const key_event_t pet_keys[] = {
        // Graphics Keyboard
        PET_KEY_DOWN_N,
        PET_KEY_RIGHT_N,
        PET_KEY_RETURN_N,
        PET_KEY_T_N,

        // Business Keyboard
        PET_KEY_DOWN_B,
        PET_KEY_RIGHT_B,
        PET_KEY_RETURN_B,
        PET_KEY_T_B,
    };
    
    static const int term_keys_unshifted[] = {
        // Graphics Keyboard
        /*   PET_KEY_DOWN_N: */ KEY_DOWN, 
        /*  PET_KEY_RIGHT_N: */ KEY_RIGHT, 
        /* PET_KEY_RETURN_N: */ '\n',
        /*      PET_KEY_T_N: */ 't',

        // Business Keyboard
        /*   PET_KEY_DOWN_B: */ KEY_DOWN,
        /*  PET_KEY_RIGHT_B: */ KEY_RIGHT,
        /* PET_KEY_RETURN_B: */ '\n',
        /*      PET_KEY_T_B: */ 't',
    };
    
    static const int term_keys_shifted[] = {
        // Graphics Keyboard
        /*   PET_KEY_DOWN_N: */ KEY_UP,
        /*  PET_KEY_RIGHT_N: */ KEY_LEFT,
        /* PET_KEY_RETURN_N: */ '\n',
        /*      PET_KEY_T_N: */ 'T',

        // Business Keyboard
        /*   PET_KEY_DOWN_B: */ KEY_UP,
        /*  PET_KEY_RIGHT_B: */ KEY_LEFT,
        /* PET_KEY_RETURN_B: */ '\n',
        /*      PET_KEY_T_B: */ 'T',
    };
    
    static_assert((ARRAY_SIZE(pet_keys) == ARRAY_SIZE(term_keys_unshifted))
        && (ARRAY_SIZE(pet_keys) == ARRAY_SIZE(term_keys_shifted)),
        "Key event arrays must be the same size");
    
    while (true) {
        key_event_t event = next_key_event(matrix);
        if (event == key_event_none) {
            return EOF;
        }

        for (size_t i = 0; i < ARRAY_SIZE(pet_keys); i++) {
            if (event == pet_keys[i]) {
                return keyscan_is_shifted(matrix)
                    ? term_keys_shifted[i]
                    : term_keys_unshifted[i];
            }
        }
    }

    return EOF;
}
