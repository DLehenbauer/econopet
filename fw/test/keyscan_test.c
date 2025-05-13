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

START_TEST(test_keyscan) {
    uint8_t matrix[KEY_COL_COUNT] = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00
    };

    for (int c = 0; c < KEY_COL_COUNT; c++) {
        for (int r = 0; r < 8; r++) {
            key_event_t expected = key_event(true, r, c);
            key_event_t actual = next_key_event(matrix);
            ck_assert_int_eq(actual, expected);
        }
    }

    ck_assert_int_eq(next_key_event(matrix), key_event_none);
    ck_assert_int_eq(next_key_event(matrix), key_event_none);

    memset(matrix, 0xFF, sizeof(matrix));
    
    for (int c = 0; c < KEY_COL_COUNT; c++) {
        for (int r = 0; r < 8; r++) {
            key_event_t expected = key_event(false, r, c);
            key_event_t actual = next_key_event(matrix);
            ck_assert_int_eq(actual, expected);
        }
    }

    ck_assert_int_eq(next_key_event(matrix), key_event_none);
    ck_assert_int_eq(next_key_event(matrix), key_event_none);
}
 
Suite *keyscan_suite(void) {
    Suite *s;
    TCase *tc_core;

    s = suite_create("keyscan");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_keyscan);
    suite_add_tcase(s, tc_core);

    return s;
}
