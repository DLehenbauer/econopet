// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input/input_actions.h"

#include "input/input.h"

tui_action_t input_menu_bind_terminal(int key) {
    switch (key) {
        case KEY_UP: return tui_action_up;
        case KEY_DOWN: return tui_action_down;
        case KEY_LEFT:
        case KEY_PGUP: return tui_action_page_up;
        case KEY_RIGHT:
        case KEY_PGDN: return tui_action_page_down;
        case '\r':
        case '\n': return tui_action_activate;
        case '\b':
        case 0x7f:
        case '\e': return tui_action_back;
        default: return tui_action_none;
    }
}

tui_action_t input_menu_bind_hid(uint8_t keycode) {
    switch (keycode) {
        case HID_KEY_ARROW_UP: return tui_action_up;
        case HID_KEY_ARROW_DOWN: return tui_action_down;
        case HID_KEY_ARROW_LEFT:
        case HID_KEY_PAGE_UP: return tui_action_page_up;
        case HID_KEY_ARROW_RIGHT:
        case HID_KEY_PAGE_DOWN: return tui_action_page_down;
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER: return tui_action_activate;
        case HID_KEY_ESCAPE:
        case HID_KEY_BACKSPACE: return tui_action_back;
        default: return tui_action_none;
    }
}
