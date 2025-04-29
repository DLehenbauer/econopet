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

#include "../src/menu/window.h"
#include "window_test.h"

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

Suite *window_suite(void) {
    Suite* s = suite_create("Window");

    TCase* test_cases = tcase_create("window");
    tcase_add_test(test_cases, test_window_create);
    tcase_add_test(test_cases, test_window_xy);
    tcase_add_test_raise_signal(test_cases, test_window_xy_x_invalid, SIGABRT);
    tcase_add_test_raise_signal(test_cases, test_window_xy_y_invalid, SIGABRT);

    tcase_add_test(test_cases, test_window_hline_valid_zero_length);
    tcase_add_test(test_cases, test_window_hline_full_buffer);

    suite_add_tcase(s, test_cases);

    return s;
}
