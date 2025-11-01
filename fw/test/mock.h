#pragma once

#include <stdint.h>

// Stub __in_flash for Linux
#define __in_flash(x)

// Mock HID keyboard report structure
// (See /opt/pico-sdk/lib/tinyusb/src/class/hid/hid.h)
typedef struct hid_keyboard_report_s {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} hid_keyboard_report_t;

// In-memory file system for testing
// Register a file with given path and content in memory
void mock_register_file(const char* path, const char* content);

// Remove a specific file from memory
void mock_unregister_file(const char* path);

// Clear all registered in-memory files
void mock_clear_files(void);
