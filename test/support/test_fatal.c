// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "test_support.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fatal.h"

#define TEST_FATAL_MESSAGE_CAPACITY 2048

static const char* expected_fatal_substring;

// Sets the diagnostic substring required by the next expected fatal error.
void test_expect_fatal_message(const char* substring) {
    expected_fatal_substring = substring;
}

// Checks expected diagnostics before aborting, including in Release builds.
void fatal(const char* const format, ...) {
    // Format the complete diagnostic before checking the test expectation.
    char message[TEST_FATAL_MESSAGE_CAPACITY];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Fail distinctly on a mismatched diagnostic instead of the expected abort.
    if (expected_fatal_substring != NULL &&
        strstr(message, expected_fatal_substring) == NULL) {
        fprintf(
            stderr,
            "fatal message did not contain '%s': %s\n",
            expected_fatal_substring,
            message
        );
        _Exit(EXIT_FAILURE);
    }

    // Emit the accepted diagnostic and preserve firmware fatal termination.
    fprintf(stderr, "fatal: %s\n", message);
    abort();
}
