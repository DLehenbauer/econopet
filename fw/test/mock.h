#pragma once

#include <stdint.h>

// Stub __in_flash for Linux or other host environments.
#define __in_flash(x)

// Mock HID keyboard report structure
// (See /opt/pico-sdk/lib/tinyusb/src/class/hid/hid.h)
typedef struct hid_keyboard_report_s {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} hid_keyboard_report_t;
