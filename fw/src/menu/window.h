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

#include "../pch.h"
 
typedef struct window_s {
    // Pointers to start/end of character buffer.
    uint8_t* const start;
    uint8_t* const end;

    // Width of window buffer.
    const unsigned int width;

    // Height of window buffer.
    const unsigned int height;
} window_t;

window_t window_create(uint8_t* pBuffer, unsigned int width, unsigned int height);

uint8_t* window_xy(const window_t* const window, unsigned int x, unsigned int y);
int window_chars_remaining(const window_t* const window, uint8_t* start);
int window_current_row(const window_t* const window, uint8_t* start);

void window_fill(const window_t* const window, uint8_t c);
uint8_t* window_hline(const window_t* const window, uint8_t* start, unsigned int length, uint8_t c);
uint8_t* window_puts_n(const window_t* const window, uint8_t* start, const char* str, unsigned int length);
uint8_t* window_puts(const window_t* const window, uint8_t* start, const char* str);
uint8_t* window_reverse(const window_t* const window, uint8_t* start, unsigned int length);
uint8_t* window_vprint(const window_t* const window, uint8_t* start, const char* const format, va_list args);
uint8_t* window_print(const window_t* const window, uint8_t* start, const char* const format, ...);
uint8_t* window_vprintln(const window_t* const window, uint8_t* start, const char* const format, va_list args);
uint8_t* window_println(const window_t* const window, uint8_t* start, const char* const format, ...);

#define CH_SPACE 0x20
