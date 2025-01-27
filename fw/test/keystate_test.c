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

#include "keystate_test.h"
#include "../src/usb/keystate.h"

START_TEST(test_keystate) {
    for (int keycode = 0; keycode < 0x100; keycode++) {
        KeyStateFlags flags = keystate_reset(keycode);
        ck_assert_int_eq(flags, 0);

        flags = keycode & (KEYSTATE_PRESSED_FLAG | KEYSTATE_SHIFTED_FLAG);
        keystate_set(keycode, flags);
    }

    for (int keycode = 0; keycode < 0x100; keycode++) {
        KeyStateFlags expected = keycode & (KEYSTATE_PRESSED_FLAG | KEYSTATE_SHIFTED_FLAG);
        ck_assert_int_eq(keystate_reset(keycode), expected);
    }

    for (int keycode = 0; keycode < 0x100; keycode++) {
        KeyStateFlags flags = keystate_reset(keycode);
        ck_assert_int_eq(flags, 0);

        flags = (~keycode) & (KEYSTATE_PRESSED_FLAG | KEYSTATE_SHIFTED_FLAG);
        keystate_set(keycode, flags);
    }

    for (int keycode = 0xFF; keycode >= 0; keycode--) {
        KeyStateFlags expected = (~keycode) & (KEYSTATE_PRESSED_FLAG | KEYSTATE_SHIFTED_FLAG);
        ck_assert_int_eq(keystate_reset(keycode), expected);
    }
}

Suite *keystate_suite(void) {
    Suite *s;
    TCase *tc_core;

    s = suite_create("KeyState");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_keystate);
    suite_add_tcase(s, tc_core);

    return s;
}
