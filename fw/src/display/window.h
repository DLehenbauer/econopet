// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdint.h>

/** Borrowed character canvas with an optional parallel color plane. */
typedef struct window_s {
    uint8_t* start;
    uint8_t* end;
    uint8_t* colors;
    unsigned int width;
    unsigned int height;
    uint8_t attr;
} window_t;

/** Create a character-only window. */
window_t window_create(uint8_t* chars, unsigned int width, unsigned int height);

/** Create a window with parallel character and attribute planes. */
window_t window_create_color(uint8_t* chars, uint8_t* colors, unsigned int width,
                             unsigned int height, uint8_t attr);

/** Return a window copy with the given active attribute. */
static inline window_t window_with_attr(const window_t* window, uint8_t attr) {
    window_t copy = *window;
    copy.attr = attr;
    return copy;
}

/** Return the character cell at an in-bounds coordinate. */
uint8_t* window_xy(const window_t* window, unsigned int x, unsigned int y);

/** Fill the window with a glyph and active attribute. */
void window_fill(const window_t* window, uint8_t glyph);

/** Fill one row with a glyph and active attribute. */
void window_fill_row(const window_t* window, unsigned int y, uint8_t glyph);

/** Write a glyph and active attribute at an in-bounds coordinate. */
void window_put(const window_t* window, unsigned int x, unsigned int y, uint8_t glyph);

/** Fill a contiguous run, returning the cell after it. */
uint8_t* window_hline(const window_t* window, uint8_t* start, unsigned int length, uint8_t glyph);

/** Draw a horizontal run in one row. */
void window_hline_at(const window_t* window, unsigned int x, unsigned int y, unsigned int length, uint8_t glyph);

/** Draw a vertical run in one column. */
void window_vline_at(const window_t* window, unsigned int x, unsigned int y, unsigned int length, uint8_t glyph);

/** Write ASCII text clipped to a single row. */
void window_text(const window_t* window, unsigned int x, unsigned int y, unsigned int max_width, const char* text);

/** Format and write ASCII text clipped to a single row. */
void window_printf(const window_t* window, unsigned int x, unsigned int y, unsigned int max_width,
                   const char* format, ...) __attribute__((format(printf, 5, 6)));

                   /** Write wrapping ASCII text, honoring newlines and ignoring carriage returns. */
uint8_t* window_puts(const window_t* window, uint8_t* start, const char* str);

/** Toggle reverse video on a contiguous run without changing attributes. */
uint8_t* window_reverse(const window_t* window, uint8_t* start, unsigned int length);

/** Toggle reverse video on a clipped row span. */
void window_reverse_at(const window_t* window, unsigned int x, unsigned int y, unsigned int length);

/** Draw a box with an optional centered title. */
void window_frame(const window_t* window, unsigned int x, unsigned int y,
                  unsigned int width, unsigned int height, const char* title);
