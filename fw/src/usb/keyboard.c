// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "keyboard.h"

#include "fatal.h"
#include "input/input_controls.h"
#include "input/input_routing.h"
#include "keyscan.h"

#define MAX_KEYBOARDS 8

/** USB transport state is independent of routed runtime key ownership. */
typedef struct {
    input_source_t source;
    bool used;
    bool attached;
    hid_keyboard_report_t previous_report;
    input_hold_t menu_hold;
} keyboard_info_t;

static keyboard_info_t keyboards[MAX_KEYBOARDS];

/** Find the raw report history for one interface. */
static keyboard_info_t* find_keyboard(uint8_t device, uint8_t instance) {
    for (size_t i = 0; i < count_of(keyboards); ++i)
        if (keyboards[i].used && keyboards[i].source.device == device
            && keyboards[i].source.instance == instance)
            return &keyboards[i];
    return NULL;
}

/** Allocate history for boot and non-boot keyboard interfaces alike. */
static keyboard_info_t* get_keyboard(uint8_t device, uint8_t instance) {
    keyboard_info_t* keyboard = find_keyboard(device, instance);
    if (keyboard != NULL)
        return keyboard;
    for (size_t i = 0; i < count_of(keyboards); ++i) {
        if (!keyboards[i].used) {
            keyboards[i] = (keyboard_info_t){.source = {device, instance}, .used = true};
            return &keyboards[i];
        }
    }
    fatal("Max supported USB keyboards is %d.", MAX_KEYBOARDS);
}

/** Recognize membership without turning repeated report observations into presses. */
static bool report_has_key(const hid_keyboard_report_t* report, uint8_t keycode) {
    for (size_t i = 0; i < count_of(report->keycode); ++i)
        if (report->keycode[i] == keycode)
            return true;
    return false;
}

/** Reflect the shared core's mapping and lock settings on a USB keyboard. */
static void sync_leds(input_source_t source) {
    static uint8_t leds;
    leds = (keyboard_core_caps_lock() ? KEYBOARD_LED_CAPSLOCK : 0)
        | (keyboard_core_model() == pet_keyboard_model_graphics ? KEYBOARD_LED_NUMLOCK : 0)
        | (keyboard_core_keymap_kind() == usb_keymap_kind_symbolic ? KEYBOARD_LED_SCROLLLOCK : 0);
    tuh_hid_set_report(source.device, source.instance, 0, HID_REPORT_TYPE_OUTPUT, &leds, sizeof(leds));
}

/** Update attached transport LEDs after the core changes mapping settings. */
static void sync_all_leds(void) {
    for (size_t i = 0; i < count_of(keyboards); ++i)
        if (keyboards[i].attached)
            sync_leds(keyboards[i].source);
}

void usb_keyboard_added(uint8_t device, uint8_t instance) {
    keyboard_info_t* const keyboard = get_keyboard(device, instance);
    if (keyboard->attached)
        fatal("USB (addr=%d, instance=%d) is already mounted.", device, instance);
    keyboard->attached = true;
    sync_leds(keyboard->source);
}

/** Cancel only this source's routed work before releasing its report history. */
static void remove_keyboard(keyboard_info_t* keyboard) {
    input_routing_cancel(keyboard->source);
    *keyboard = (keyboard_info_t){0};
}

void usb_keyboard_removed(uint8_t device, uint8_t instance) {
    keyboard_info_t* const keyboard = find_keyboard(device, instance);
    if (keyboard == NULL || !keyboard->attached)
        fatal("USB (addr=%d, instance=%d) not found on removal.", device, instance);
    remove_keyboard(keyboard);
}

void keyboard_source_removed(uint8_t device, uint8_t instance) {
    keyboard_info_t* const keyboard = find_keyboard(device, instance);
    if (keyboard != NULL && !keyboard->attached)
        remove_keyboard(keyboard);
}

void process_kbd_report(uint8_t device, uint8_t instance, const hid_keyboard_report_t* report) {
    keyboard_info_t* const keyboard = get_keyboard(device, instance);
    const hid_keyboard_report_t previous = keyboard->previous_report;
    const uint8_t shifts = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    bool other_key = (report->modifier & (uint8_t)~shifts) != 0;
    for (size_t i = 0; i < count_of(report->keycode); ++i)
        other_key |= report->keycode[i] != 0;
    input_chord_update(keyboard->source, &keyboard->menu_hold,
        (report->modifier & shifts) == shifts, other_key);

    // Retain raw state even when the router suppresses presses in the menu.
    keyboard->previous_report = *report;
    keyboard_edge_t edges[count_of(previous.keycode) + count_of(report->keycode)];
    size_t edge_count = 0;
    for (size_t i = 0; i < count_of(previous.keycode); ++i) {
        const uint8_t keycode = previous.keycode[i];
        if (keycode && !report_has_key(report, keycode))
            edges[edge_count++] = (keyboard_edge_t){keycode, false};
    }
    for (size_t i = 0; i < count_of(report->keycode); ++i) {
        const uint8_t keycode = report->keycode[i];
        if (keycode && !report_has_key(&previous, keycode))
            edges[edge_count++] = (keyboard_edge_t){keycode, true};
    }
    const keyboard_batch_t batch = {
        .source = keyboard->source,
        .modifiers_pressed = report->modifier & (uint8_t)~previous.modifier,
        .modifiers_released = previous.modifier & (uint8_t)~report->modifier,
        .edges = edges,
        .edge_count = edge_count,
    };
    input_routing_keyboard(&batch);
}

void usb_keyboard_poll_gestures(void) {
    for (size_t i = 0; i < count_of(keyboards); ++i) {
        if (keyboards[i].used && input_hold_ready(&keyboards[i].menu_hold)) {
            input_routing_gesture(keyboards[i].source, input_gesture_dual_shift);
            input_hold_consume(&keyboards[i].menu_hold);
        }
    }
}

void usb_keyboard_consume_gestures(void) {
    for (size_t i = 0; i < count_of(keyboards); ++i)
        if (keyboards[i].used)
            input_hold_consume(&keyboards[i].menu_hold);
}

void dispatch_key_events(void) {
    if (keyboard_core_dispatch())
        sync_all_leds();
}
