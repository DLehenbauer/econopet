#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MOCK_RAM_SIZE 0x10000

extern uint8_t mock_ram[MOCK_RAM_SIZE];

void mock_reset(void);
void mock_breakpoint_set_hit_addr(uint16_t addr);
bool mock_breakpoint_halt_was_cleared(void);

void mock_ieee_enqueue_rx(bool atn, uint8_t byte);
size_t mock_ieee_data_count(void);
uint8_t mock_ieee_data_byte(size_t index);
bool mock_ieee_data_eoi(size_t index);
void mock_ieee_clear_data(void);
size_t mock_ieee_status_count(void);
uint8_t mock_ieee_status_byte(size_t index);
bool mock_ieee_status_eoi(size_t index);
void mock_ieee_clear_status(void);

// Stub Pico SDK types and macros for non-Pico builds
#define __in_flash(x) x
#define __not_in_flash_func(x) x
typedef unsigned int uint;
typedef uint64_t absolute_time_t;

// Mock HID keyboard report structure
// (See /opt/pico-sdk/lib/tinyusb/src/class/hid/hid.h)
typedef struct hid_keyboard_report_s {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} hid_keyboard_report_t;

uint64_t time_us_64(void);
absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t time);

// In-memory file system for testing
// Register a file with given path and content in memory
void mock_register_file(const char* path, const char* content);

// Register a mutable or read-only binary file for code that uses stdio.
void mock_register_binary_file(const char* path, const void* data, size_t size,
                               bool writable);

// Remove a specific file from memory
void mock_unregister_file(const char* path);

// Clear all registered in-memory files
void mock_clear_files(void);

// Require the next fatal message to contain the given text.
void mock_expect_fatal_message(const char* substring);
