// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "window.h"

#include "display/char_encoding.h"
#include "display/display.h"

window_t window_create(uint8_t* chars, unsigned int width, unsigned int height) {
    assert(chars != NULL);
    return (window_t){.start = chars,
                      .end = chars + (size_t)width * height,
                      .width = width,
                      .height = height,
                      .attr = VIDEO_DEFAULT_ATTRIBUTE};
}

window_t window_create_color(uint8_t* chars, uint8_t* colors, unsigned int width, unsigned int height, uint8_t attr) {
    assert(chars != NULL && colors != NULL);
    window_t window = window_create(chars, width, height);
    window.colors = colors;
    window.attr = attr;
    return window;
}

/** Check a contiguous span in the borrowed character plane. */
static void check_span(const window_t* window, uint8_t* start, unsigned int length) {
    (void)window;
    (void)start;
    (void)length;

    assert(start >= window->start && start < window->end);
    assert(length <= (size_t)(window->end - start));
}

uint8_t* window_xy(const window_t* window, unsigned int x, unsigned int y) {
    assert(x < window->width && y < window->height);

    return window->start + (size_t)y * window->width + x;
}

void window_fill(const window_t* window, uint8_t glyph) {
    const size_t size = (size_t)window->width * window->height;
    memset(window->start, glyph, size);
    if (window->colors != NULL) {
        memset(window->colors, window->attr, size);
    }
}

uint8_t* window_hline(const window_t* window, uint8_t* start, unsigned int length, uint8_t glyph) {
    check_span(window, start, length);
    memset(start, glyph, length);
    if (window->colors != NULL) {
        memset(window->colors + (start - window->start), window->attr, length);
    }
    return start + length;
}

void window_hline_at(const window_t* window, unsigned int x, unsigned int y, unsigned int length, uint8_t glyph) {
    assert(y < window->height && x <= window->width && length <= window->width - x);
    if (length != 0) {
        window_hline(window, window_xy(window, x, y), length, glyph);
    }
}

void window_fill_row(const window_t* window, unsigned int y, uint8_t glyph) {
    window_hline_at(window, 0, y, window->width, glyph);
}

void window_put(const window_t* window, unsigned int x, unsigned int y, uint8_t glyph) {
    window_hline(window, window_xy(window, x, y), /* length: */ 1, glyph);
}

void window_vline_at(const window_t* window, unsigned int x, unsigned int y, unsigned int length, uint8_t glyph) {
    assert(x < window->width && y <= window->height && length <= window->height - y);
    for (unsigned int row = 0; row < length; ++row) {
        window_put(window, x, y + row, glyph);
    }
}

void window_text(const window_t* window, unsigned int x, unsigned int y, unsigned int max_width, const char* text) {
    assert(window != NULL && text != NULL);
    if (x >= window->width || y >= window->height || max_width == 0) {
        return;
    }
    const unsigned int limit = MIN(max_width, window->width - x);
    for (unsigned int i = 0; i < limit && text[i] != '\0'; ++i) {
        window_put(window, x + i, y, ascii_to_vrom((uint8_t)text[i]));
    }
}

void window_printf(const window_t* window, unsigned int x, unsigned int y, unsigned int max_width, const char* format,
                   ...) {
    char line[DISPLAY_COLS_80 + 1];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    window_text(window, x, y, max_width, line);
}

uint8_t* window_puts(const window_t* window, uint8_t* start, const char* str) {
    bool wrapped = false;
    while (*str != '\0' && start < window->end) {
        if (*str == '\n') {
            if (wrapped) {
                wrapped = false;
            } else {
                const size_t offset = (size_t)(start - window->start);
                const size_t next_row = ((offset / window->width) + 1) * window->width;
                const size_t size = (size_t)(window->end - window->start);
                start = window->start + MIN(next_row, size);
            }
        } else if (*str != '\r') {
            if (window->colors != NULL) {
                window->colors[start - window->start] = window->attr;
            }
            *start++ = ascii_to_vrom((uint8_t)*str);
            wrapped = start < window->end && (size_t)(start - window->start) % window->width == 0;
        }
        ++str;
    }
    return start;
}

uint8_t* window_reverse(const window_t* window, uint8_t* start, unsigned int length) {
    check_span(window, start, length);
    while (length--) {
        *start++ ^= VROM_REVERSE_MASK;
    }
    return start;
}

void window_reverse_at(const window_t* window, unsigned int x, unsigned int y, unsigned int length) {
    if (x >= window->width || y >= window->height || length == 0) {
        return;
    }
    window_reverse(window, window_xy(window, x, y), MIN(length, window->width - x));
}

void window_frame(const window_t* window, unsigned int x, unsigned int y, unsigned int width, unsigned int height,
                  const char* title) {
    assert(width >= 2 && height >= 2);
    assert(x <= window->width && width <= window->width - x);
    assert(y <= window->height && height <= window->height - y);
    window_hline_at(window, x + 1, y, width - 2, VROM_HLINE);
    window_hline_at(window, x + 1, y + height - 1, width - 2, VROM_HLINE);
    window_vline_at(window, x, y + 1, height - 2, VROM_VLINE);
    window_vline_at(window, x + width - 1, y + 1, height - 2, VROM_VLINE);
    window_put(window, x, y, VROM_CORNER_TOP_LEFT);
    window_put(window, x + width - 1, y, VROM_CORNER_TOP_RIGHT);
    window_put(window, x, y + height - 1, VROM_CORNER_BOTTOM_LEFT);
    window_put(window, x + width - 1, y + height - 1, VROM_CORNER_BOTTOM_RIGHT);
    if (title != NULL && title[0] != '\0' && width > 4) {
        const unsigned int len = (unsigned int)MIN(strlen(title), (size_t)width - 4);
        const unsigned int title_x = x + (width - len) / 2;
        window_put(window, title_x - 1, y, VROM_SPACE);
        window_text(window, title_x, y, len, title);
        window_put(window, title_x + len, y, VROM_SPACE);
    }
}
