// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "test_support_test.h"

#include <signal.h>
#include <stdint.h>

#include "fatal.h"
#include "sd/sd.h"
#include "test_support.h"

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

// Requires allocation failure to abort rather than returning a null placeholder.
START_TEST(test_failed_allocation) {
    vetted_malloc(SIZE_MAX);
}
END_TEST

// Register host fixture and allocation checks for the forked runner.
Suite* test_support_suite(void) {
    Suite* const suite = suite_create("TestSupport");
    TCase* const fixtures = tcase_create("fixtures");
    tcase_add_test_raise_signal(fixtures, test_register_relative_path, SIGABRT);
    tcase_add_test_raise_signal(fixtures, test_register_duplicate_path, SIGABRT);
    tcase_add_test_raise_signal(fixtures, test_open_relative_path, SIGABRT);
    suite_add_tcase(suite, fixtures);

    TCase* const allocation = tcase_create("allocation");
    tcase_add_test_raise_signal(allocation, test_failed_allocation, SIGABRT);
    suite_add_tcase(suite, allocation);
    return suite;
}
