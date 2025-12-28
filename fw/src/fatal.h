/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

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
