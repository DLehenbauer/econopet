#pragma once

#include <stdbool.h>
#include <stdint.h>

// Stub Pico SDK types and macros for non-Pico builds
#define __in_flash(x) x
#define __not_in_flash_func(x) x
typedef unsigned int uint;

// Mock HID keyboard report structure
// (See /opt/pico-sdk/lib/tinyusb/src/class/hid/hid.h)
typedef struct hid_keyboard_report_s {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} hid_keyboard_report_t;

// Mock Pico SDK functions
void __wfi();
void tight_loop_contents(void);
void watchdog_enable(unsigned int delay_ms, bool pause_on_debug);

// In-memory file system for testing
// Register a file with given path and content in memory
void mock_register_file(const char* path, const char* content);

// Remove a specific file from memory
void mock_unregister_file(const char* path);

// Clear all registered in-memory files
void mock_clear_files(void);
