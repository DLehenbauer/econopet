// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "window_test.h"

#include <limits.h>
#include <signal.h>

#include "display/char_encoding.h"
#include "display/window.h"

#define WIDTH 5
#define HEIGHT 3
#define BUFFER_SIZE (WIDTH * HEIGHT)

uint8_t storage[BUFFER_SIZE + 2]; // Extra space for boundary checks
uint8_t* buffer = storage + 1; // Pointer to the start of the buffer

#define TOP 0
#define LEFT 0
#define BOTTOM (HEIGHT - 1)
#define RIGHT (WIDTH - 1)

#define TL 0
#define TR 4
#define BL 10
#define BR 14

void ck_buffer_overflow() {
    ck_assert_msg(storage[0] == 0xFE, "Buffer overflow detected: 0x%02X", buffer[0]);
    ck_assert_msg(storage[BUFFER_SIZE + 1] == 0xFE, "Buffer overflow detected: 0x%02X", buffer[BUFFER_SIZE + 1]);
}

void ck_buffer_eq(const uint8_t* const expected) {
    ck_buffer_overflow();
    
    // Check the buffer contents against the expected values
    for (unsigned int y = 0; y < HEIGHT; y++) {
        for (unsigned int x = 0; x < WIDTH; x++) {
            size_t index = y * WIDTH + x;
            
            ck_assert_msg(
                buffer[index] == expected[index], 
                "Mismatch at position (%u, %u): expected 0x%02X, got 0x%02X", 
                y, x, expected[index], buffer[index]
            );
        }
    }
}

window_t create_test_window() {
    memset(buffer, 0x00, BUFFER_SIZE);  // Clear the buffer
    storage[0] = 0xFE;                  // Underflow fence
    storage[BUFFER_SIZE + 1] = 0xFE;    // Overflow fence

    return window_create(buffer, WIDTH, HEIGHT);
}

START_TEST(test_window_create) {
    window_t window = window_create(buffer, WIDTH, HEIGHT);

    ck_assert_ptr_eq(window.start, buffer);
    ck_assert_ptr_eq(window.end, buffer + BUFFER_SIZE);
    ck_assert_int_eq(window.width, WIDTH);
    ck_assert_int_eq(window.height, HEIGHT);
} END_TEST

START_TEST(test_window_xy) {
    window_t window = create_test_window();
    uint8_t* current = window.start;

    // Test all valid cordinates
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            ck_assert_ptr_eq(window_xy(&window, x, y), current++);
        }
    }
} END_TEST

START_TEST(test_window_xy_x_invalid) {
    window_t window = create_test_window();
    
    window_xy(&window, WIDTH, 0);
} END_TEST

START_TEST(test_window_xy_y_invalid) {
    window_t window = create_test_window();
    
    window_xy(&window, 0, HEIGHT);
} END_TEST

START_TEST(test_window_fill) {
    window_t window = create_test_window();

    // Fill the window with a specific value
    uint8_t fill_value = 0xAA;
    window_fill(&window, fill_value);

    // Verify that the entire buffer is filled with the specified value
    for (unsigned int i = 0; i < BUFFER_SIZE; i++) {
        ck_assert_uint_eq(buffer[i], fill_value);
    }
} END_TEST

START_TEST(test_window_hline_valid_zero_length) {
    window_t window = create_test_window();

    // Test zero-length line in bottom-right corner
    uint8_t* start = window_xy(&window, RIGHT, BOTTOM);
    uint8_t* result = window_hline(&window, start, /* length: */ 0, 0xAA);
    ck_assert_ptr_eq(result, start); // Ensure the return pointer is correct

    // Verify the buffer contents
    ck_assert_uint_eq(buffer[BR], 0x00); // Ensure no change
} END_TEST

START_TEST(test_window_hline_full_buffer) {
    window_t window = create_test_window();

    // Test zero-length line in bottom-right corner
    uint8_t* start = window_xy(&window, 0, 0);
    uint8_t* result = window_hline(&window, start, /* length: */ BUFFER_SIZE, 0xAA);
    ck_assert_ptr_eq(result, start + BUFFER_SIZE);

    // Verify the buffer contents
    ck_assert_uint_eq(buffer[0],               0xAA); // Ensure no change
    ck_assert_uint_eq(buffer[BUFFER_SIZE - 1], 0xAA); // Ensure no change
} END_TEST

START_TEST(test_window_puts_handles_newlines) {
    window_t window = create_test_window();

    uint8_t* result = window_puts(&window, window_xy(&window, 2, 0), "AB\nCD");

    ck_assert_ptr_eq(result, window_xy(&window, 2, 1));
    ck_assert_uint_eq(buffer[2], 'A');
    ck_assert_uint_eq(buffer[3], 'B');
    ck_assert_uint_eq(buffer[5], 'C');
    ck_assert_uint_eq(buffer[6], 'D');
    ck_assert_uint_eq(buffer[4], 0x00);
    ck_buffer_overflow();
} END_TEST

START_TEST(test_window_puts_ignores_carriage_returns) {
    window_t window = create_test_window();

    uint8_t* result = window_puts(&window, window.start, "A\rB");

    ck_assert_ptr_eq(result, window.start + 2);
    ck_assert_uint_eq(buffer[0], 'A');
    ck_assert_uint_eq(buffer[1], 'B');
    ck_buffer_overflow();
} END_TEST

START_TEST(test_window_puts_newline_after_full_row_does_not_skip_row) {
    window_t window = create_test_window();

    uint8_t* result = window_puts(&window, window.start, "ABCDE\r\nF");

    ck_assert_ptr_eq(result, window.start + 6);
    ck_assert_mem_eq(buffer, "ABCDEF", 6);
    ck_assert_uint_eq(buffer[2 * WIDTH], 0x00);
    ck_buffer_overflow();
} END_TEST

// Rejects null window backing storage in Debug tests.
START_TEST(test_window_null_storage) {
    window_create(NULL, 1, 1);
}
END_TEST

// Checks length before overflowing pointer arithmetic or writing any bytes.
START_TEST(test_window_excessive_length) {
    uint8_t storage = 0;
    const window_t window = window_create(&storage, 1, 1);
    window_hline(&window, window.start, UINT_MAX, 0);
}
END_TEST

/** Check color writes, clipping, reverse video, and zero-length edge runs. */
START_TEST(test_color_canvas_and_clipping) {
    uint8_t chars[BUFFER_SIZE + 2], colors[BUFFER_SIZE + 2];
    memset(chars, 0xee, sizeof(chars));
    memset(colors, 0xee, sizeof(colors));
    const window_t window = window_create_color(chars + 1, colors + 1, WIDTH, HEIGHT, 0x12);
    const window_t styled = window_with_attr(&window, 0x34);
    window_fill(&window, VROM_SPACE);
    window_fill_row(&styled, 1, VROM_HLINE);
    window_put(&styled, 0, 0, VROM_SHADE);
    window_hline(&styled, window_xy(&styled, 1, 0), 2, VROM_HLINE);
    window_vline_at(&styled, 4, 0, HEIGHT, VROM_VLINE);
    window_text(&styled, 3, 1, 9, "ABC");
    window_printf(&styled, 1, 2, 2, "%s", "XYZ");
    window_puts(&styled, window_xy(&styled, 0, 2), "Q");
    window_hline_at(&styled, WIDTH, 0, 0, VROM_SPACE);
    window_vline_at(&styled, 0, HEIGHT, 0, VROM_SPACE);
    window_text(&styled, WIDTH, 0, 1, "X");
    window_text(&styled, 0, HEIGHT, 1, "X");
    window_text(&styled, 0, 0, 0, "X");
    ck_assert_uint_eq(chars[1 + WIDTH + 3], ascii_to_vrom('A'));
    ck_assert_uint_eq(chars[1 + WIDTH + 4], ascii_to_vrom('B'));
    ck_assert_uint_eq(chars[1 + 2 * WIDTH], ascii_to_vrom('Q'));
    ck_assert_uint_eq(chars[1 + 2 * WIDTH + 1], ascii_to_vrom('X'));
    ck_assert_uint_eq(chars[1 + 2 * WIDTH + 2], ascii_to_vrom('Y'));
    ck_assert_uint_eq(chars[1 + 2 * WIDTH + 3], VROM_SPACE);
    ck_assert_uint_eq(colors[1 + 0], 0x34);
    ck_assert_uint_eq(colors[1 + 1], 0x34);
    ck_assert_uint_eq(colors[1 + WIDTH], 0x34);
    ck_assert_uint_eq(colors[1 + 2 * WIDTH + 4], 0x34);
    ck_assert_uint_eq(colors[1 + 2 * WIDTH], 0x34);
    ck_assert_uint_eq(colors[1 + 2 * WIDTH + 1], 0x34);
    ck_assert_uint_eq(colors[1 + WIDTH + 4], 0x34);
    ck_assert_uint_eq(colors[1 + 2 * WIDTH + 3], 0x12);
    window_reverse_at(&styled, 3, 1, 10);
    window_reverse(&styled, window_xy(&styled, 0, 0), 1);
    ck_assert_uint_eq(chars[1 + WIDTH + 3], ascii_to_vrom('A') ^ VROM_REVERSE_MASK);
    ck_assert_uint_eq(colors[1 + WIDTH + 3], 0x34);
    ck_assert_uint_eq(colors[1], 0x34);
    ck_assert_uint_eq(chars[0], 0xee);
    ck_assert_uint_eq(chars[sizeof(chars) - 1], 0xee);
    ck_assert_uint_eq(colors[0], 0xee);
    ck_assert_uint_eq(colors[sizeof(colors) - 1], 0xee);
}
END_TEST

/** Frame borders and the title use the active attribute. */
START_TEST(test_colored_frame) {
    uint8_t chars[10 * 4], colors[10 * 4];
    const window_t window = window_create_color(chars, colors, 10, 4, 0x67);
    window_fill(&window, VROM_SPACE);
    window_frame(&window, 0, 0, 10, 4, "Hi");
    ck_assert_uint_eq(chars[0], VROM_CORNER_TOP_LEFT);
    ck_assert_uint_eq(chars[9], VROM_CORNER_TOP_RIGHT);
    ck_assert_uint_eq(chars[10], VROM_VLINE);
    ck_assert_uint_eq(chars[30], VROM_CORNER_BOTTOM_LEFT);
    ck_assert_uint_eq(chars[39], VROM_CORNER_BOTTOM_RIGHT);
    ck_assert_uint_eq(chars[3], VROM_SPACE);
    ck_assert_uint_eq(chars[4], ascii_to_vrom('H'));
    ck_assert_uint_eq(chars[5], ascii_to_vrom('i'));
    ck_assert_uint_eq(chars[6], VROM_SPACE);
    for (size_t i = 0; i < sizeof(colors); ++i) ck_assert_uint_eq(colors[i], 0x67);
}
END_TEST

Suite *window_suite(void) {
    Suite* s = suite_create("Window");

    TCase* test_cases = tcase_create("window");
    tcase_add_test(test_cases, test_window_create);
    tcase_add_test(test_cases, test_window_fill);
    tcase_add_test(test_cases, test_window_xy);
    tcase_add_test_raise_signal(test_cases, test_window_xy_x_invalid, SIGABRT);
    tcase_add_test_raise_signal(test_cases, test_window_xy_y_invalid, SIGABRT);
    tcase_add_test_raise_signal(test_cases, test_window_null_storage, SIGABRT);
    tcase_add_test_raise_signal(test_cases, test_window_excessive_length, SIGABRT);

    tcase_add_test(test_cases, test_window_hline_valid_zero_length);
    tcase_add_test(test_cases, test_window_hline_full_buffer);
    tcase_add_test(test_cases, test_window_puts_handles_newlines);
    tcase_add_test(test_cases, test_window_puts_ignores_carriage_returns);
    tcase_add_test(test_cases, test_window_puts_newline_after_full_row_does_not_skip_row);
    tcase_add_test(test_cases, test_color_canvas_and_clipping);
    tcase_add_test(test_cases, test_colored_frame);

    suite_add_tcase(s, test_cases);

    return s;
}
