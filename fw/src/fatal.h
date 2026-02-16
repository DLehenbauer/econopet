// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stddef.h>

void __attribute__((noreturn)) fatal(const char* const format, ...);
void* vetted_malloc(size_t __size);

/**
 * Firmware assertion macro. If the condition is false, displays the formatted
 * error message on the PET's native display, HDMI output, and serial terminal,
 * then halts until the user presses [RETURN] or the menu button.
 */
#define vet(cond, fmt, ...) \
    do { if (!(cond)) fatal(fmt, ##__VA_ARGS__); } while (0)
