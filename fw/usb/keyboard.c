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

uint8_t key_matrix[KEY_COL_COUNT] = {
    /* 0 */ 0xff,
    /* 1 */ 0xff,
    /* 2 */ 0xff,
    /* 3 */ 0xff,
    /* 4 */ 0xff,
    /* 5 */ 0xff,
    /* 6 */ 0xff,
    /* 7 */ 0xff,
    /* 8 */ 0xff,
    /* 9 */ 0xff,
};

typedef struct __attribute__((packed)) {
    unsigned int rowMask: 8;    // 1-bit mask for rows 0-7
    unsigned int col: 4;        // 4-bit column index for cols 0-9
    unsigned int reserved: 2;
    unsigned int deshift: 1;    // 0 = Normal, 1 = Implicitly remove shift
    unsigned int shift: 1;      // 0 = Normal, 1 = Implicitly add shift
} KeyInfo;

static const KeyInfo s_keymap[] = {
    #include "../keymaps/grus_pos.h"
};

static bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }

    return false;
}

void key_down(uint8_t keycode) {
    KeyInfo const key = s_keymap[keycode];
    uint8_t rowMask = key.rowMask;
    uint8_t col = key.col;

    if (col == 15) {
        printf("USB: Key down %d=(undefined)\n", keycode);
        return;
    }

    if (key_matrix[col] & rowMask) {
        key_matrix[col] &= ~rowMask;
        printf("USB: Key down: %d=(%d,%d)\n", keycode, ffs(rowMask) - 1, col);
    }
}

void key_up(uint8_t keycode) {
    KeyInfo const key = s_keymap[keycode];
    uint8_t rowMask = key.rowMask;
    uint8_t col = key.col;

    if (col == 15) {
        printf("USB: Key up %d=(undefined)\n", keycode);
        return;
    }

    if (key_matrix[col] & ~rowMask) {
        key_matrix[col] |= rowMask;
        printf("USB: Key up: %d=(%d,%d)\n", keycode, ffs(rowMask) - 1, col);
    }
}

void process_kbd_report(hid_keyboard_report_t const* report) {
    static hid_keyboard_report_t prev_report = {0, 0, {0}};

    uint8_t current_modifiers = report->modifier;
    uint8_t previous_modifiers = prev_report.modifier;

    for (uint8_t i = 0; i < 8; i++) {
        uint8_t current_modifier = current_modifiers & 0x01;
        uint8_t previous_modifier = previous_modifiers & 0x01;

        if (current_modifier) {
            if (!previous_modifier) {
                key_down(HID_KEY_CONTROL_LEFT + i);
            }
        } else if (previous_modifier) {
            key_up(HID_KEY_CONTROL_LEFT + i);
        }

        current_modifiers >>= 1;
        previous_modifiers >>= 1;
    }

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = prev_report.keycode[i];
        if (keycode && !find_key_in_report(report, keycode)) {
            key_up(keycode);
        }
    }

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_key_in_report(&prev_report, keycode)) {
            key_down(keycode);
        }
    }

    prev_report = *report;
}
