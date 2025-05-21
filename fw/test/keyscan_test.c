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

#include "keyscan_test.h"
#include "usb/keyscan.h"
#include "usb/keyboard.h"
#include "term.h"

START_TEST(test_keyscan_next_key_event) {
    uint8_t matrix[KEY_COL_COUNT] = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00
    };

    for (int c = 0; c < KEY_COL_COUNT; c++) {
        for (int r = 0; r < 8; r++) {
            key_event_t expected = PET_KEY_EVENT(true, r, c);
            key_event_t actual = next_key_event(matrix);
            ck_assert_int_eq(actual, expected);
        }
    }

    ck_assert_int_eq(next_key_event(matrix), key_event_none);
    ck_assert_int_eq(next_key_event(matrix), key_event_none);

    memset(matrix, 0xFF, sizeof(matrix));
    
    for (int c = 0; c < KEY_COL_COUNT; c++) {
        for (int r = 0; r < 8; r++) {
            key_event_t expected = PET_KEY_EVENT(false, r, c);
            key_event_t actual = next_key_event(matrix);
            ck_assert_int_eq(actual, expected);
        }
    }

    ck_assert_int_eq(next_key_event(matrix), key_event_none);
    ck_assert_int_eq(next_key_event(matrix), key_event_none);
}

void press_key(uint8_t matrix[KEY_COL_COUNT], key_event_t key) {
    unsigned int row = PET_KEY_ROW(key);
    unsigned int col = PET_KEY_COL(key);

    assert(row < 8);
    assert(col < KEY_COL_COUNT);

    matrix[col] &= ~(1 << row);
}

void release_key(uint8_t matrix[KEY_COL_COUNT], key_event_t key) {
    unsigned int row = PET_KEY_ROW(key);
    unsigned int col = PET_KEY_COL(key);

    assert(row < 8);
    assert(col < KEY_COL_COUNT);

    matrix[col] |= (1 << row);
}

START_TEST(test_keyscan_getch) {
    uint8_t matrix[KEY_COL_COUNT] = {
        0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff
    };

    ck_assert_int_eq(keyscan_getch(matrix), EOF);

    static const key_event_t pet_keys[] = {
        // Graphics
        PET_KEY_DOWN_N,
        PET_KEY_RIGHT_N,
        PET_KEY_RETURN_N,

        // Business
        PET_KEY_DOWN_B,
        PET_KEY_RIGHT_B,
        PET_KEY_RETURN_B,
    };

    static const int expected_keys_unshifted[] = {
        // Graphics
        KEY_DOWN,
        KEY_RIGHT,
        '\n',

        // Business
        KEY_DOWN,
        KEY_RIGHT,
        '\n',
    };

    static_assert(ARRAY_SIZE(pet_keys) == ARRAY_SIZE(expected_keys_unshifted),
        "Key event arrays must be the same size");
    
    for (size_t i = 0; i < ARRAY_SIZE(pet_keys); i++) {
        press_key(matrix, pet_keys[i]);
        ck_assert_int_eq(keyscan_getch(matrix), expected_keys_unshifted[i]);
        release_key(matrix, pet_keys[i]);
    }

    static const key_event_t shift_keys[] = {
        PET_KEY_LSHIFT_N,
        PET_KEY_LSHIFT_B,
        PET_KEY_RSHIFT_N,
        PET_KEY_RSHIFT_B,
    };

    static const int expected_keys_shifted[] = {
        // Graphics
        KEY_UP,
        KEY_LEFT,
        '\n',

        // Business
        KEY_UP,
        KEY_LEFT,
        '\n',
    };

    static_assert(ARRAY_SIZE(expected_keys_shifted) == ARRAY_SIZE(expected_keys_unshifted),
        "Key event arrays must be the same size");

    for (size_t i = 0; i < ARRAY_SIZE(shift_keys); i++) {
        press_key(matrix, shift_keys[i]);
        for (size_t j = 0; j < ARRAY_SIZE(expected_keys_shifted); j++) {
            press_key(matrix, pet_keys[j]);
            ck_assert_int_eq(keyscan_getch(matrix), expected_keys_shifted[j]);
            release_key(matrix, pet_keys[j]);
        }

        release_key(matrix, shift_keys[i]);
    }
}

Suite *keyscan_suite(void) {
    Suite *s;
    TCase *tc_core;

    s = suite_create("keyscan");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_keyscan_next_key_event);
    tcase_add_test(tc_core, test_keyscan_getch);
    suite_add_tcase(s, tc_core);

    return s;
}
