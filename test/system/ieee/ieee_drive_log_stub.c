// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "diag/log/log.h"

// Writes buffered-firmware DEBUG diagnostics to the host test's captured stderr.
void log_debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

// Writes firmware informational logs emitted by the GoogleTest system runner.
void log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

// Fail if the hardware fatal path requests retained logs in this stderr-only fixture.
uint32_t log_entry_count(log_level_t min_level) {
    (void)min_level;
    assert(false && "retained log history is unavailable in the system-test logger");
    abort();
}

// Fail if the hardware fatal path tries to dump logs in this stderr-only fixture.
uint32_t log_dump(log_level_t min_level) {
    (void)min_level;
    assert(false && "log dumping is unavailable in the system-test logger");
    abort();
}
