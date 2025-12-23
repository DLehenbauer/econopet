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

#include "window.h"
#include "char_encoding.h"
#include "dvi/dvi.h"

// Character ROM offsets
#define CH_SPACE 0x20 // ' ' (space)

window_t window_create(uint8_t* start, unsigned int width, unsigned int height) {
    assert(start != NULL);

    return (window_t){.start = start, .end = start + (width * height), .width = width, .height = height};
}

static inline void check_start(const window_t* const window, uint8_t* start) {
    (void)window;
    (void)start;

    assert(window->start <= start && start < window->end);
}

static inline void check_length(const window_t* const window, uint8_t* start, unsigned int length) {
    (void)window;
    
    start += length;

    assert(window->start <= start && start <= window->end);
}

uint8_t* window_xy(const window_t* const window, unsigned int x, unsigned int y) {
    const unsigned int width = window->width;
    const unsigned int height = window->height;

    (void)height;

    assert(y < height);
    assert(x < width);

    return &window->start[y * width + x];
}

unsigned int window_char_position(const window_t* const window, uint8_t* start) {
    check_start(window, start);

    const size_t delta = ((void*)start - (void*)window->start);
    assert(delta < UINT_MAX);

    return (unsigned int)delta;
}

int window_chars_remaining(const window_t* const window, uint8_t* start) {
    check_start(window, start);

    const int delta = (int)(((void*)window->end) - ((void*)start));
    assert(INT_MIN < delta && delta < INT_MAX);

    return delta;
}

int window_current_row(const window_t* const window, uint8_t* start) {
    check_start(window, start);

    const int row = (int)window_char_position(window, start) / window->width;
    assert(INT_MIN < row && row < INT_MAX);

    return row;
}

void window_fill(const window_t* const window, uint8_t c) { memset(window->start, c, window->width * window->height); }

uint8_t* window_hline(const window_t* const window, uint8_t* start, unsigned int length, uint8_t c) {
    check_start(window, start);
    check_length(window, start, length);

    memset(start, c, length);

    return start + length;
}

uint8_t* window_hline3(const window_t* const window, uint8_t* start, uint8_t length, uint8_t left, uint8_t middle,
                       uint8_t right) {
    check_start(window, start);
    check_length(window, start, length);

    *start++ = left;

    length -= 2;
    while (length--) {
        *start++ = middle;
    }

    *start++ = right;

    return start;
}

uint8_t* window_fill_rect(const window_t* const window, uint8_t* start, uint8_t width, uint8_t height, uint8_t c) {
    while (height--) {
        window_hline(window, start, width, c);
        start += window->width;
    }

    return start;
}

// Write a null-terminated string to the window buffer. The string is truncated
// if it exceeds the the given length or the bounds of the window.
//
// The function returns the pointer to the next position in the window buffer after the string.
uint8_t* window_puts_n(const window_t* const window, uint8_t* start, const char* const str, unsigned int length) {
    uint8_t* pCh = (uint8_t*)str;

    while (*pCh != 0 && length > 0 && start < window->end) {
        *start++ = ascii_to_vrom(*(pCh++));
        length--;
    }

    if (*pCh != 0) {
        fprintf(stderr, "Warning: string '%s' truncated to %u characters\n", str, length);
    }

    return start;
}

// Write a null-terminated string to the window buffer. The string is truncated
// if it exceeds the window size.
//
// The function returns the pointer to the next position in the window buffer after the string.
uint8_t* window_puts(const window_t* const window, uint8_t* start, const char* str) {
    return window_puts_n(window, start, str, /* length: */ window_chars_remaining(window, window->start));
}

uint8_t* window_vprint(const window_t* const window, uint8_t* start, const char* const format, va_list args) {
    static char buffer[VIDEO_CHAR_BUFFER_BYTE_SIZE];

    const unsigned int remaining = window_chars_remaining(window, start);
    assert(remaining <= sizeof(buffer));

    size_t written = vsnprintf(buffer, remaining, format, args);
    window_puts(window, start, buffer);

    return start + written;
}

uint8_t* window_print(const window_t* const window, uint8_t* start, const char* const format, ...) {
    va_list args;
    va_start(args, format);
    start = window_vprint(window, start, format, args);
    va_end(args);

    return start;
}

uint8_t* window_vprintln(const window_t* const window, uint8_t* start, const char* const format, va_list args) {
    start = window_vprint(window, start, format, args);
    start = window_xy(window, 0, window_current_row(window, start) + 1);
    return start;
}

uint8_t* window_println(const window_t* const window, uint8_t* start, const char* const format, ...) {
    va_list args;
    va_start(args, format);
    start = window_vprintln(window, start, format, args);
    va_end(args);

    return start;
}

uint8_t* window_reverse(const window_t* const window, uint8_t* start, unsigned int length) {
    check_start(window, start);
    check_length(window, start, length);

    while (length--) {
        *start++ ^= 0x80;
    }

    return start;
}
