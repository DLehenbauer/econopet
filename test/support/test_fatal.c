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
static void (*expected_fatal_callback)(void);

// Sets the diagnostic substring required by the next expected fatal error.
void test_expect_fatal_message(const char* substring) {
    expected_fatal_substring = substring;
}

// Registers a one-shot check of state at the fatal boundary.
void test_expect_fatal_callback(void (*callback)(void)) { expected_fatal_callback = callback; }

// Checks production fatal diagnostics before aborting in host tests.
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

    // Inspect cleanup before aborting, without reusing the hook on nested errors.
    if (expected_fatal_callback != NULL) {
        void (*callback)(void) = expected_fatal_callback;
        expected_fatal_callback = NULL;
        callback();
    }

    // Emit the accepted diagnostic and preserve firmware fatal termination.
    fprintf(stderr, "fatal: %s\n", message);
    abort();
}
