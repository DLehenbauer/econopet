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
    int number_failed = 0;

    // These tests are run in the same process for convenient debugging.
    SRunner* sr1 = srunner_create(keyscan_suite());
    srunner_add_suite(sr1, keystate_suite());
    srunner_set_fork_status(sr1, CK_NOFORK);
    srunner_run_all(sr1, CK_VERBOSE);
    number_failed += srunner_ntests_failed(sr1);
    srunner_free(sr1);

    // These tests are run in a separate process as they intentionally assert.
    SRunner* sr2 = srunner_create(window_suite());
    srunner_run_all(sr2, CK_VERBOSE);
    number_failed += srunner_ntests_failed(sr2);
    srunner_free(sr2);

    return (number_failed == 0) ? 0 : 1;
}

int main(void) {
    // config_test();
    // menu();

    return run_suite();
}
