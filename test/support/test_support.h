// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pico SDK declarations needed by firmware running in host tests.
#ifndef count_of
// Matches the Pico SDK array element count for builds without SDK headers.
#define count_of(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef MIN
// Returns the smaller value (arguments may be evaluated more than once).
#define MIN(a, b) ((b) > (a) ? (a) : (b))
#endif

// Leaves flash-resident declarations unchanged on the host.
#define __in_flash(x) x
// Leaves RAM-resident function names unchanged on the host.
#define __not_in_flash_func(x) x
typedef unsigned int uint;
typedef uint64_t absolute_time_t;

#define TEST_HID_KEYBOARD_KEY_COUNT 6

// Host representation of a TinyUSB HID keyboard report.
typedef struct hid_keyboard_report_s {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[TEST_HID_KEYBOARD_KEY_COUNT];
} hid_keyboard_report_t;

// Returns host monotonic time in microseconds.
uint64_t time_us_64(void);

// Returns host monotonic time using the Pico SDK time representation.
absolute_time_t get_absolute_time(void);

// Converts a host timestamp to milliseconds.
uint32_t to_ms_since_boot(absolute_time_t time);

// Registers a read-only text file in the shared in-memory filesystem.
void test_register_file(const char* path, const char* content);

// Registers a mutable or read-only binary file for code that uses stdio.
void test_register_binary_file(const char* path, const void* data, size_t size,
                               bool writable);

// Removes one registered file after all of its handles have been closed.
void test_unregister_file(const char* path);

// Removes all registered files after all of their handles have been closed.
void test_clear_files(void);

// Requires a subsequent fatal message to contain the given text.
void test_expect_fatal_message(const char* substring);
