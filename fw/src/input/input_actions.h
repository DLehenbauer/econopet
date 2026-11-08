// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Identifies an input producer independently of its current destination. */
typedef struct {
    uint8_t device;   ///< USB address, or zero for non-USB input.
    uint8_t instance; ///< USB interface, or the non-USB source identifier.
} input_source_t;

#define INPUT_SOURCE_UART ((input_source_t){0, 0})
#define INPUT_SOURCE_PET ((input_source_t){0, 1})
#define INPUT_SOURCE_BUTTON ((input_source_t){0, 2})

/** Shared menu operations, bound before configurable PET mapping. */
typedef enum {
    tui_action_none,
    tui_action_up,
    tui_action_down,
    tui_action_page_up,
    tui_action_page_down,
    tui_action_activate,
    tui_action_back,
} tui_action_t;

/** Recognized controls are bound using the current Running/Menu destination. */
typedef enum {
    input_gesture_button_short,
    input_gesture_button_long,
    input_gesture_dual_shift,
} input_gesture_t;

/** A decoded physical edge uses HID's key vocabulary, not its report transport. */
typedef struct {
    uint8_t keycode;
    bool pressed;
} keyboard_edge_t;

/** Borrowed edges are applied synchronously as one source's modifier/key batch. */
typedef struct {
    input_source_t source;
    uint8_t modifiers_pressed;
    uint8_t modifiers_released;
    const keyboard_edge_t* edges;
    size_t edge_count;
} keyboard_batch_t;

/** Machine intent is latched separately so it cannot wait behind navigation. */
typedef enum {
    input_system_none,
    input_system_request_reset,
    input_system_request_menu,
} input_system_action_t;

/** Provenance supports cancellation and arbitration, not action interpretation. */
typedef struct {
    input_source_t source;
    input_system_action_t action;
} input_system_request_t;

/** Decode terminal navigation and its aliases without applying a PET keymap. */
tui_action_t input_menu_bind_terminal(int key);
/** Bind HID menu aliases independently of the configured runtime mapping. */
tui_action_t input_menu_bind_hid(uint8_t keycode);
