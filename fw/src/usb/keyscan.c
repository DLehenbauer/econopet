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

#include "keyboard.h"
#include "keyscan.h"
#include "term.h"

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

int keyscan_is_shifted(uint8_t matrix[KEY_COL_COUNT]) {
    static const key_event_t shift_keys[] = {
        PET_KEY_LSHIFT_N,
        PET_KEY_LSHIFT_B,
        PET_KEY_RSHIFT_N,
        PET_KEY_RSHIFT_B,
    };

    for (size_t i = 0; i < ARRAY_SIZE(shift_keys); i++) {
        const unsigned int row = PET_KEY_ROW(shift_keys[i]);
        const unsigned int col = PET_KEY_COL(shift_keys[i]);
        
        if ((matrix[col] & (1 << row)) == 0) {
            return true;
        }
    }

    return false;
}

int keyscan_getch(uint8_t matrix[KEY_COL_COUNT]) {
    static const key_event_t gfx_keys[] = {
        PET_KEY_DOWN_N,
        PET_KEY_RIGHT_N,
        PET_KEY_RETURN_N,
    };
    
    static const key_event_t business_keys[] = {
        PET_KEY_DOWN_B,
        PET_KEY_RIGHT_B,
        PET_KEY_RETURN_B,
    };
    
    static const int term_keys_unshifted[] = {
        KEY_DOWN,
        KEY_RIGHT,
        '\n',
    };
    
    static const int term_keys_shifted[] = {
        KEY_UP,
        KEY_LEFT,
        '\n',
    };
    
    static_assert((ARRAY_SIZE(gfx_keys) == ARRAY_SIZE(business_keys))
        && (ARRAY_SIZE(gfx_keys) == ARRAY_SIZE(term_keys_unshifted))
        && (ARRAY_SIZE(gfx_keys) == ARRAY_SIZE(term_keys_shifted)),
        "Key event arrays must be the same size");
    
    while (true) {
        key_event_t event = next_key_event(matrix);
        if (event == key_event_none) {
            return EOF;
        }

        for (size_t i = 0; i < ARRAY_SIZE(gfx_keys); i++) {
            if (event == gfx_keys[i]) {
                return keyscan_is_shifted(matrix) ? term_keys_shifted[i] : term_keys_unshifted[i];
            } else if (event == business_keys[i]) {
                return keyscan_is_shifted(matrix) ? term_keys_shifted[i] : term_keys_unshifted[i];
            }
        }
    }

    return EOF;
}
