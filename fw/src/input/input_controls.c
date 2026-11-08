// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input/input_controls.h"

#include "input/input_routing.h"
#include "usb/keyboard.h"
#include "usb/keyscan.h"

#define BUTTON_DEBOUNCE_US 50000u

// Ignore power-on button capacitor charging until the first stable release.
static input_hold_t button = {.held = true, .blocked = true};
static bool button_raw = true;
static uint64_t button_changed;

void input_hold_update(input_hold_t* const hold, const bool eligible) {
    if (!eligible) {
        hold->held = false;
        hold->blocked = false;
    } else if (!hold->held) {
        hold->held = true;
        hold->since = time_us_64();
    }
}

bool input_hold_ready(const input_hold_t* const hold) {
    return hold->held && !hold->blocked && time_us_64() - hold->since >= INPUT_LONG_PRESS_US;
}

void input_hold_consume(input_hold_t* const hold) {
    hold->blocked = hold->held;
}

void input_chord_update(input_source_t source, input_hold_t* hold, bool both_shifts, bool other_key) {
    // Preserve intent if the threshold was crossed before a release or contaminating key.
    if ((!both_shifts || other_key) && input_hold_ready(hold))
        input_routing_gesture(source, input_gesture_dual_shift);
    input_hold_update(hold, both_shifts);
    if (both_shifts && other_key)
        input_hold_consume(hold);
}

void input_button_update(const bool pressed) {
    const uint64_t now = time_us_64();
    if (pressed != button_raw) {
        button_raw = pressed;
        button_changed = now;
    }
    if (now - button_changed < BUTTON_DEBOUNCE_US || pressed == button.held)
        return;
    if (!pressed && !button.blocked)
        input_routing_gesture(INPUT_SOURCE_BUTTON,
            input_hold_ready(&button) ? input_gesture_button_long : input_gesture_button_short);
    input_hold_update(&button, pressed);
}

void input_controls_poll(void) {
    usb_keyboard_poll_gestures();
    keyscan_poll_gestures();
    if (input_hold_ready(&button)) {
        input_routing_gesture(INPUT_SOURCE_BUTTON, input_gesture_button_long);
        input_hold_consume(&button);
    }
}

void input_controls_consume(void) {
    usb_keyboard_consume_gestures();
    keyscan_consume_gestures();
    input_hold_consume(&button);
}
