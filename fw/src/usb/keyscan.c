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

// Sentinel for "no key event". (Note that column '0xF' exceeds KEY_COL_COUNT)
const key_event_t key_event_none = 0x7F;

key_event_t key_event(bool pressed, uint8_t row, uint8_t col) {
    assert(row < 8);
    assert(col < KEY_COL_COUNT);
    return (pressed ? 0x80 : 0x00) | row << 4 | col;
}

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
            event = key_event(/* pressed: */ true, r, c);
            previous[c] &= ~mask;
        } else if ((matrix[c] & mask) != 0 && (previous[c] & mask) == 0) {
            // Key is released and was previously pressed
            event = key_event(/* pressed: */ false, r, c);
            previous[c] |= mask;
        }
        
        next_rc(&r, &c);
    } while (event == key_event_none && (r != start_r || c != start_c));

    return event;
}
