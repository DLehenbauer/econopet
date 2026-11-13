// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdint.h>

#include "input/keyboard_core.h"

/** Attach a boot keyboard interface and initialize its LEDs. */
void usb_keyboard_added(uint8_t dev_addr, uint8_t instance);
/** Remove an attached interface and cancel only its owned input. */
void usb_keyboard_removed(uint8_t dev_addr, uint8_t instance);
/** Retire report state for an unmounted non-boot HID interface. */
void keyboard_source_removed(uint8_t dev_addr, uint8_t instance);
/** Decode raw HID edges and gestures for the shared router. */
void process_kbd_report(uint8_t dev_addr, uint8_t instance, hid_keyboard_report_t const* report);
/** Emit qualified USB menu requests independently of navigation. */
void usb_keyboard_poll_gestures(void);
/** Suppress currently held USB chords until release. */
void usb_keyboard_consume_gestures(void);
/** Dispatch a core modifier group and synchronize attached USB LEDs if needed. */
void dispatch_key_events(void);
