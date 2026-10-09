// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "global_test.h"

#include <signal.h>

#include "global.h"

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

// Register scratch-buffer ownership checks for the forked runner.
Suite* global_suite(void) {
    Suite* const suite = suite_create("Global");
    TCase* const checks = tcase_create("temp_buffer");
    tcase_add_test_raise_signal(checks, test_double_acquire, SIGABRT);
    tcase_add_test_raise_signal(checks, test_release_without_acquire, SIGABRT);
    tcase_add_test_raise_signal(checks, test_release_foreign_buffer, SIGABRT);
    tcase_add_test_raise_signal(checks, test_release_null_owner, SIGABRT);
    suite_add_tcase(suite, checks);
    return suite;
}
