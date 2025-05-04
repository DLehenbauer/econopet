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
#include "../video/video.h"

// Character ROM offsets
#define CH_SPACE 0x20 // ' ' (space)

// Lower-case (POKE 59468,14)
// This table maps from ASCII to the character ROM offsets.
static const uint8_t ascii_to_vrom[] = {
    /*              0               1               2               3               4               5               6               7               8               9               A               B               C               D               E               F  */
    /* 0 | NUL */ 0x00, /* SOH */ 0x01, /* STX */ 0x02, /* ETX */ 0x03, /* EOT */ 0x04, /* ENQ */ 0x05, /* ACK */ 0x06, /* BEL */ 0x07, /*  BS */ 0x08, /*  HT */ 0x09, /*  LF */ 0x0A, /*  VT */ 0x0B, /*  FF */ 0x0C, /*  CR */ 0x0D, /*  SO */ 0x0E, /*  SI */ 0x0F,
    /* 1 | DLE */ 0x10, /* DC1 */ 0x11, /* DC2 */ 0x12, /* DC3 */ 0x13, /* DC4 */ 0x14, /* NAK */ 0x15, /* SYN */ 0x16, /* ETB */ 0x17, /* CAN */ 0x18, /*  EM */ 0x19, /* SUB */ 0x1A, /* ESC */ 0x1B, /*  FS */ 0x1C, /*  GS */ 0x1D, /*  RS */ 0x1E, /*  US */ 0x1F,
    /* 2 |  SP */ 0x20, /*   ! */ 0x21, /*   " */ 0x22, /*   # */ 0x23, /*   $ */ 0x24, /*   % */ 0x25, /*   & */ 0x26, /*   ' */ 0x27, /*   ( */ 0x28, /*   ) */ 0x29, /*   * */ 0x2A, /*   + */ 0x2B, /*   , */ 0x2C, /*   - */ 0x2D, /*   . */ 0x2E, /*   / */ 0x2F,
    /* 3 |   0 */ 0x30, /*   1 */ 0x31, /*   2 */ 0x32, /*   3 */ 0x33, /*   4 */ 0x34, /*   5 */ 0x35, /*   6 */ 0x36, /*   7 */ 0x37, /*   8 */ 0x38, /*   9 */ 0x39, /*   : */ 0x3A, /*   ; */ 0x3B, /*   < */ 0x3C, /*   = */ 0x3D, /*   > */ 0x3E, /*   ? */ 0x3F,
    /* 4 |   @ */ 0x00, /*   A */ 0x41, /*   B */ 0x42, /*   C */ 0x43, /*   D */ 0x44, /*   E */ 0x45, /*   F */ 0x46, /*   G */ 0x47, /*   H */ 0x48, /*   I */ 0x49, /*   J */ 0x4A, /*   K */ 0x4B, /*   L */ 0x4C, /*   M */ 0x4D, /*   N */ 0x4E, /*   O */ 0x4F,
    /* 5 |   P */ 0x50, /*   Q */ 0x51, /*   R */ 0x52, /*   S */ 0x53, /*   T */ 0x54, /*   U */ 0x55, /*   V */ 0x56, /*   W */ 0x57, /*   X */ 0x58, /*   Y */ 0x59, /*   Z */ 0x5A, /*   [ */ 0x1B, /*   \ */ 0x1C, /*   ] */ 0x1D, /*   ^ */ 0x1E, /*   _ */ 0x64,
    /* 6 |   ` */ 0x27, /*   a */ 0x01, /*   b */ 0x02, /*   c */ 0x03, /*   d */ 0x04, /*   e */ 0x05, /*   f */ 0x06, /*   g */ 0x07, /*   h */ 0x08, /*   i */ 0x09, /*   j */ 0x0A, /*   k */ 0x0B, /*   l */ 0x0C, /*   m */ 0x0D, /*   n */ 0x0E, /*   o */ 0x0F,
    /* 7 |   p */ 0x10, /*   q */ 0x11, /*   r */ 0x12, /*   s */ 0x13, /*   t */ 0x14, /*   u */ 0x15, /*   v */ 0x16, /*   w */ 0x17, /*   x */ 0x18, /*   y */ 0x19, /*   z */ 0x1A, /*   { */ 0x6B, /*   | */ 0x5B, /*   } */ 0x73, /*   ~ */ 0x71, /* DEL */ 0x7F,
};

window_t window_create(uint8_t* start, unsigned int width, unsigned int height) {
    assert(start != NULL);

    return (window_t) {
        .start = start,
        .end = start + (width * height),
        .width = width,
        .height = height
    };
}

static inline void check_start(const window_t* const window, uint8_t* start) {
    assert(window->start <= start && start < window->end);
}

static inline void check_length(const window_t* const window, uint8_t* start, unsigned int length) {
    start += length;
    
    assert(window->start <= start && start <= window->end);
}

uint8_t* window_xy(const window_t* const window, unsigned int x, unsigned int y) {
    const unsigned int width = window->width;
    const unsigned int height = window->height;

    assert(y < height);
    assert(x < width);

    return &window->start[y * width + x];
}

void window_fill(const window_t* const window, uint8_t c) {
    memset(window->start, c, window->width * window->height);
}

uint8_t* window_hline(const window_t* const window, uint8_t* start, unsigned int length, uint8_t c) {
    check_start(window, start);
    check_length(window, start, length);

    memset(start, c, length);

    return start + length;
}

uint8_t* window_hline3(const window_t* const window, uint8_t* start, uint8_t length, uint8_t left, uint8_t middle, uint8_t right) {
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
uint8_t* window_puts_n(const window_t* const window, uint8_t* start, const char* str, unsigned int length) {
    uint8_t* pCh = (uint8_t*) str;

    while (*pCh != 0 && length > 0 && start < window->end) {
        *start++ = ascii_to_vrom[*(pCh++)];
        length--;
    }

    return start;
}

// Write a null-terminated string to the window buffer. The string is truncated
// if it exceeds the window size.
//
// The function returns the pointer to the next position in the window buffer after the string.
uint8_t* window_puts(const window_t* const window, uint8_t* start, const char* str) {
    return window_puts_n(window, start, str, /* length: */ UINT_MAX);
}

uint8_t* window_reverse(const window_t* const window, uint8_t* start, unsigned int length) {
    check_start(window, start);
    check_length(window, start, length);

    while (length--) {
        *start++ ^= 0x80;
    }

    return start;
}
