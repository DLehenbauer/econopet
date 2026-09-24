// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stddef.h>

// Format a fatal message into fixed storage, display it, then halt.
void fatal(const char* const format, ...)
    __attribute__((format(printf, 1, 2), noreturn));

void* vetted_malloc(size_t __size);

/**
 * Firmware assertion macro. If the condition is false, displays the formatted
 * error message on the PET's native display, HDMI output, and serial terminal,
 * then halts.
 */
#define vet(cond, fmt, ...) \
    do { if (!(cond)) fatal(fmt, ##__VA_ARGS__); } while (0)

/**
 * Verify that 'index' is in the range [0, length).  Calls fatal() on failure.
 */
#define vet_index(index, length) \
    vet((size_t)(index) < (size_t)(length), \
        "index out of range: %zu (length %zu)", (size_t)(index), (size_t)(length))
