// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <check.h>
#include "breakpoint_test.h"
#include "char_encoding_test.h"
#include "config_parser_test.h"
#include "crtc_test.h"
#include "cpu_test.h"
#include "fatal_error_test.h"
#include "diskimage_test.h"
#include "display_test.h"
#include "dos_media_test.h"
#include "dos_rel_test.h"
#include "dos_test.h"
#include "global_test.h"
#include "fpga_image_test.h"
#include "ieee_drive_test.h"
#include "input_test.h"
#include "keyscan_test.h"
#include "keyboard_test.h"
#include "keystate_test.h"
#include "log_test.h"
#include "window_test.h"
#include "petscii_test.h"
#include "sd_activity_test.h"
#include "tape_dir_test.h"
#include "system_state_test.h"
#include "test_support_test.h"
#include "tui_test.h"
#include "menu_browser_test.h"
#include "menu_ui_test.h"
#include "menu_session_test.h"

int run_suite() {
    int number_failed = 0;

    // These tests are run in the same process for convenient debugging.
    SRunner* sr1 = srunner_create(breakpoint_suite());
    srunner_add_suite(sr1, char_encoding_suite());
    srunner_add_suite(sr1, config_parser_suite());
    srunner_add_suite(sr1, crtc_suite());
    srunner_add_suite(sr1, display_suite());
    srunner_add_suite(sr1, diskimage_suite());
    srunner_add_suite(sr1, dos_media_suite());
    srunner_add_suite(sr1, dos_rel_suite());
    srunner_add_suite(sr1, dos_suite());
    srunner_add_suite(sr1, fpga_image_suite());
    srunner_add_suite(sr1, ieee_drive_suite());
    srunner_add_suite(sr1, keyscan_suite());
    srunner_add_suite(sr1, keyboard_suite());
    srunner_add_suite(sr1, input_suite());
    srunner_add_suite(sr1, keystate_suite());
    srunner_add_suite(sr1, log_suite());
    srunner_add_suite(sr1, petscii_suite());
    srunner_add_suite(sr1, sd_activity_suite());
    srunner_add_suite(sr1, system_state_suite());
    srunner_add_suite(sr1, tape_dir_suite());
    srunner_add_suite(sr1, tui_suite());
    srunner_add_suite(sr1, menu_browser_suite());
    srunner_add_suite(sr1, menu_ui_suite());
    srunner_add_suite(sr1, menu_session_suite());
    srunner_set_fork_status(sr1, CK_NOFORK);
    srunner_run_all(sr1, CK_VERBOSE);
    number_failed += srunner_ntests_failed(sr1);
    srunner_free(sr1);

    // These tests need process isolation for fatal paths or one-time startup state.
    SRunner* sr2 = srunner_create(breakpoint_fatal_suite());
    srunner_add_suite(sr2, cpu_suite());
    srunner_add_suite(sr2, fatal_error_suite());
    srunner_add_suite(sr2, config_parser_fatal_suite());
    srunner_add_suite(sr2, ieee_drive_fatal_suite());
    srunner_add_suite(sr2, global_suite());
    srunner_add_suite(sr2, log_fatal_suite());
    srunner_add_suite(sr2, menu_browser_fatal_suite());
    srunner_add_suite(sr2, menu_ui_fatal_suite());
    srunner_add_suite(sr2, test_support_suite());
    srunner_add_suite(sr2, tui_fatal_suite());
    srunner_add_suite(sr2, window_suite());
    srunner_run_all(sr2, CK_VERBOSE);
    number_failed += srunner_ntests_failed(sr2);
    srunner_free(sr2);

    return (number_failed == 0) ? 0 : 1;
}

int main(void) {
    return run_suite();
}
