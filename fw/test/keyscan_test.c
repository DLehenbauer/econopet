// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "keyscan_test.h"
#include "input/keyboard_core.h"
#include "usb/keyscan.h"
#include "input/input.h"
#include "system/state.h"

START_TEST(test_keyscan_next_key_event) {
    uint8_t matrix[ECONOPET_KBD_COL_COUNT] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    for (int c = 0; c < ECONOPET_KBD_COL_COUNT; c++) {
        for (int r = 0; r < ECONOPET_KBD_ROW_COUNT; r++) {
            key_event_t expected = PET_KEY_EVENT(true, r, c);
            key_event_t actual = next_key_event(matrix);
            ck_assert_int_eq(actual, expected);
        }
    }

    ck_assert_int_eq(next_key_event(matrix), key_event_none);
    ck_assert_int_eq(next_key_event(matrix), key_event_none);

    memset(matrix, 0xFF, sizeof(matrix));
    
    for (int c = 0; c < ECONOPET_KBD_COL_COUNT; c++) {
        for (int r = 0; r < ECONOPET_KBD_ROW_COUNT; r++) {
            key_event_t expected = PET_KEY_EVENT(false, r, c);
            key_event_t actual = next_key_event(matrix);
            ck_assert_int_eq(actual, expected);
        }
    }

    ck_assert_int_eq(next_key_event(matrix), key_event_none);
    ck_assert_int_eq(next_key_event(matrix), key_event_none);
}

void press_key(uint8_t matrix[ECONOPET_KBD_COL_COUNT], key_event_t key) {
    unsigned int row = PET_KEY_ROW(key);
    unsigned int col = PET_KEY_COL(key);

    ck_assert_uint_lt(row, ECONOPET_KBD_ROW_COUNT);
    ck_assert_uint_lt(col, ECONOPET_KBD_COL_COUNT);

    matrix[col] &= ~(1 << row);
}

void release_key(uint8_t matrix[ECONOPET_KBD_COL_COUNT], key_event_t key) {
    unsigned int row = PET_KEY_ROW(key);
    unsigned int col = PET_KEY_COL(key);

    ck_assert_uint_lt(row, ECONOPET_KBD_ROW_COUNT);
    ck_assert_uint_lt(col, ECONOPET_KBD_COL_COUNT);

    matrix[col] |= (1 << row);
}

START_TEST(test_keyscan_navigation) {
    const pet_keyboard_model_t saved_model = system_state.pet_keyboard_model;
    uint8_t matrix[ECONOPET_KBD_COL_COUNT] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    ck_assert_int_eq(keyscan_navigation(matrix), tui_action_none);

    static const key_event_t pet_keys[] = {
        // Graphics
        PET_KEY_DOWN_N,
        PET_KEY_RIGHT_N,
        PET_KEY_RETURN_N,
        PET_KEY_RUNSTOP_N,
        PET_KEY_DELETE_N,

        // Business
        PET_KEY_DOWN_B,
        PET_KEY_RIGHT_B,
        PET_KEY_RETURN_B,
        PET_KEY_RUNSTOP_B,
        PET_KEY_DELETE_B,
    };

    static const int expected_keys_unshifted[] = {
        // Graphics
        KEY_DOWN,
        KEY_RIGHT,
        '\n',
        '\e',
        '\b',

        // Business
        KEY_DOWN,
        KEY_RIGHT,
        '\n',
        '\e',
        '\b',
    };

    static_assert(count_of(pet_keys) == count_of(expected_keys_unshifted),
        "Key event arrays must be the same size");
    
    for (size_t i = 0; i < count_of(pet_keys); i++) {
        system_state.pet_keyboard_model = i < count_of(pet_keys) / 2
            ? pet_keyboard_model_graphics : pet_keyboard_model_business;
        press_key(matrix, pet_keys[i]);
        ck_assert_int_eq(keyscan_navigation(matrix), input_menu_bind_terminal(expected_keys_unshifted[i]));
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
        '\e',
        '\b',

        // Business
        KEY_UP,
        KEY_LEFT,
        '\n',
        '\e',
        '\b',
    };

    static_assert(count_of(expected_keys_shifted) == count_of(expected_keys_unshifted),
        "Key event arrays must be the same size");

    for (size_t i = 0; i < count_of(shift_keys); i++) {
        system_state.pet_keyboard_model = i % 2 == 0
            ? pet_keyboard_model_graphics : pet_keyboard_model_business;
        press_key(matrix, shift_keys[i]);
        const size_t first = i % 2 == 0 ? 0 : count_of(pet_keys) / 2;
        for (size_t j = first; j < first + count_of(pet_keys) / 2; j++) {
            press_key(matrix, pet_keys[j]);
            ck_assert_int_eq(keyscan_navigation(matrix), input_menu_bind_terminal(expected_keys_shifted[j]));
            release_key(matrix, pet_keys[j]);
        }

        release_key(matrix, shift_keys[i]);
    }
    memset(matrix, 0xff, sizeof(matrix));
    keyscan_reset(matrix);
    system_state.pet_keyboard_model = saved_model;
}

Suite *keyscan_suite(void) {
    Suite *s;
    TCase *tc_core;

    s = suite_create("keyscan");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_keyscan_next_key_event);
    tcase_add_test(tc_core, test_keyscan_navigation);
    suite_add_tcase(s, tc_core);

    return s;
}
