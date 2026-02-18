// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <check.h>
#include "breakpoint_test.h"
#include "char_encoding_test.h"
#include "config_parser_test.h"
#include "crtc_test.h"
#include "keyscan_test.h"
#include "keystate_test.h"
#include "log_test.h"
#include "window_test.h"

int run_suite() {
    int number_failed = 0;

    // These tests are run in the same process for convenient debugging.
    SRunner* sr1 = srunner_create(breakpoint_suite());
    srunner_add_suite(sr1, char_encoding_suite());
    srunner_add_suite(sr1, config_parser_suite());
    srunner_add_suite(sr1, crtc_suite());
    srunner_add_suite(sr1, keyscan_suite());
    srunner_add_suite(sr1, keystate_suite());
    srunner_add_suite(sr1, log_suite());
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
    return run_suite();
}
