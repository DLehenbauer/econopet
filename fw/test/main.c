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

#include <check.h>
#include "keyscan_test.h"
#include "keystate_test.h"
#include "config_test.h"
#include "menu_test.h"
#include "window_test.h"

int run_suite() {
    SRunner* sr = srunner_create(keyscan_suite());
    srunner_add_suite(sr, keystate_suite());
    srunner_add_suite(sr, window_suite());

    // To aide debugging, run tests in the current process.
    // srunner_set_fork_status(sr, CK_NOFORK);

    srunner_run_all(sr, CK_NORMAL);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? 0 : 1;
}

int main(void) {
    config_test();
    // menu();

    return run_suite();
}
