// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "diag/log/log.h"

// Writes firmware informational logs emitted by the GoogleTest system runner.
void log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}
