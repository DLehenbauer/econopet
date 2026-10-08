// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "invariant_test.h"

#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include "diag/log/log.h"
#include "display/window.h"
#include "fatal.h"
#include "global.h"
#include "sd/sd.h"
#include "test_support.h"

// Rejects acquiring shared scratch storage while it already has an owner.
START_TEST(test_double_acquire) {
    acquire_temp_buffer();
    acquire_temp_buffer();
}
END_TEST

// Rejects releasing shared scratch storage that was never acquired.
START_TEST(test_release_without_acquire) {
    uint8_t* buffer = NULL;
    release_temp_buffer(&buffer);
}
END_TEST

// Rejects a foreign buffer instead of clearing its ownership pointer.
START_TEST(test_release_foreign_buffer) {
    uint8_t byte = 0;
    uint8_t* buffer = &byte;
    acquire_temp_buffer();
    release_temp_buffer(&buffer);
}
END_TEST

// Rejects a null owner pointer before dereferencing it.
START_TEST(test_release_null_owner) {
    acquire_temp_buffer();
    release_temp_buffer(NULL);
}
END_TEST

// Rejects a fixture path outside the absolute-path namespace.
START_TEST(test_register_relative_path) {
    test_register_file("relative.txt", "");
}
END_TEST

// Rejects a duplicate fixture before allocating or publishing another entry.
START_TEST(test_register_duplicate_path) {
    test_register_file("/invariant.txt", "");
    test_register_file("/invariant.txt", "");
}
END_TEST

// Applies the same path invariant to fixture opens as to registration.
START_TEST(test_open_relative_path) {
    sd_open("relative.txt", "r");
}
END_TEST

// Rejects null window backing storage in Debug tests.
START_TEST(test_window_null_storage) {
    window_create(NULL, 1, 1);
}
END_TEST

// Checks length before overflowing pointer arithmetic or writing any bytes.
START_TEST(test_window_excessive_length) {
    uint8_t storage = 0;
    const window_t window = window_create(&storage, 1, 1);
    window_hline(&window, window.start, UINT_MAX, 0);
}
END_TEST

// Rejects both ends of the invalid log-level domain before indexing its rings.
START_TEST(test_invalid_log_level) {
    const log_level_t levels[] = { LOG_LEVEL_COUNT, (log_level_t)-1 };
    log_event(levels[_i], "rejected");
}
END_TEST

// Requires allocation failure to abort rather than returning a null placeholder.
START_TEST(test_failed_allocation) {
    vetted_malloc(SIZE_MAX);
}
END_TEST

// Check each expected assertion failure in its own Debug child process.
Suite* invariant_suite(void) {
    Suite* const suite = suite_create("invariants");
    TCase* const checks = tcase_create("paranoid_checks");
    tcase_add_test_raise_signal(checks, test_double_acquire, SIGABRT);
    tcase_add_test_raise_signal(checks, test_release_without_acquire, SIGABRT);
    tcase_add_test_raise_signal(checks, test_release_foreign_buffer, SIGABRT);
    tcase_add_test_raise_signal(checks, test_release_null_owner, SIGABRT);
    tcase_add_test_raise_signal(checks, test_register_relative_path, SIGABRT);
    tcase_add_test_raise_signal(checks, test_register_duplicate_path, SIGABRT);
    tcase_add_test_raise_signal(checks, test_open_relative_path, SIGABRT);
    tcase_add_test_raise_signal(checks, test_window_null_storage, SIGABRT);
    tcase_add_test_raise_signal(checks, test_window_excessive_length, SIGABRT);
    tcase_add_loop_test_raise_signal(checks, test_invalid_log_level, SIGABRT, 0, 2);
    tcase_add_test_raise_signal(checks, test_failed_allocation, SIGABRT);
    suite_add_tcase(suite, checks);
    return suite;
}
