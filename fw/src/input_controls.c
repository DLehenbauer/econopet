// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input_controls.h"

#include "input.h"
#include "usb/keyboard.h"

#define BUTTON_DEBOUNCE_US 50000u

// Ignore power-on button capacitor charging until the first stable release.
static input_hold_t button = {.held = true, .blocked = true};
static bool button_raw = true;
static uint64_t button_changed;
static int button_release = EOF;

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

void input_button_update(const bool pressed) {
    const uint64_t now = time_us_64();
    if (pressed != button_raw) {
        button_raw = pressed;
        button_changed = now;
    }
    if (now - button_changed < BUTTON_DEBOUNCE_US || pressed == button.held)
        return;
    if (!pressed && !button.blocked)
        button_release = input_hold_ready(&button) ? KEY_BTN_LONG : KEY_BTN_SHORT;
    input_hold_update(&button, pressed);
}

int input_controls_getch(void) {
    const bool chord = keyboard_menu_requested();
    const bool long_press = input_hold_ready(&button);
    const int key = long_press ? KEY_BTN_LONG : button_release;
    if (key != EOF || chord) {
        input_controls_consume();
        return key != EOF ? key : KEY_SHIFT_SHIFT;
    }
    return EOF;
}

void input_controls_consume(void) {
    keyboard_menu_consume();
    input_hold_consume(&button);
    button_release = EOF;
}
