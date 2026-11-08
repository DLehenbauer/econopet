// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input/input_routing.h"

#include "diag/log/log.h"
#include "fatal.h"
#include "input/input.h"
#include "input/input_controls.h"
#include "input/keyboard_core.h"
#include "input/term_inject.h"
#include "ui/cli.h"
#include "usb/keyscan.h"

#define MENU_QUEUE_LOG2_CAPACITY 4
#define MENU_QUEUE_CAPACITY (1u << MENU_QUEUE_LOG2_CAPACITY)
#define MENU_QUEUE_CAPACITY_MASK (MENU_QUEUE_CAPACITY - 1u)
#define TERMINAL_CTRL_C 0x03

/** A decoded press retains its producer until it is consumed or cancelled. */
typedef struct {
    input_source_t source;
    tui_action_t action;
} menu_input_t;

static menu_input_t menu_queue[MENU_QUEUE_CAPACITY];
static unsigned int head, tail;
static bool menu_active;
static input_system_request_t system_request;
static bool request_replaceable;

void input_routing_init(void) {
    system_request = (input_system_request_t){0};
    request_replaceable = false;
    input_routing_set_menu_active(false);
}

void input_routing_begin_cycle(void) {
    request_replaceable = system_request.action == input_system_none;
}

void input_routing_end_cycle(void) {
    request_replaceable = false;
}

void input_routing_reset(void) {
    // Cancel input in one place, keeping adapter report history for held-key suppression.
    head = tail = 0;
    term_inject_cancel();
    keyboard_core_cancel_all();
    keyscan_reset(pet_key_matrix);
    input_controls_consume();
}

bool input_routing_menu_active(void) {
    return menu_active;
}

void input_routing_set_menu_active(bool active) {
    // Transfer the destination and retire navigation and runtime ownership together.
    menu_active = active;
    input_routing_reset();
}

bool input_routing_submit(input_source_t source, tui_action_t action) {
    if (!menu_active || action == tui_action_none)
        return false;
    const unsigned int next = (head + 1) & MENU_QUEUE_CAPACITY_MASK;
    if (next == tail) {
        log_warn("Menu input queue full (source %u:%u)",
            (unsigned int)source.device, (unsigned int)source.instance);
        return false;
    }
    menu_queue[head] = (menu_input_t){source, action};
    head = next;
    return true;
}

void input_routing_cancel(input_source_t source) {
    if (source.device == INPUT_SOURCE_UART.device && source.instance == INPUT_SOURCE_UART.instance)
        term_inject_cancel();
    keyboard_core_cancel(source);
    // Compact unread entries without changing the order of surviving producers.
    unsigned int write = tail;
    for (unsigned int read = tail; read != head; read = (read + 1) & MENU_QUEUE_CAPACITY_MASK) {
        const menu_input_t event = menu_queue[read];
        if (event.source.device != source.device || event.source.instance != source.instance) {
            menu_queue[write] = event;
            write = (write + 1) & MENU_QUEUE_CAPACITY_MASK;
        }
    }
    head = write;
}

void input_routing_terminal(int key) {
    if (system_state.terminal_session == terminal_session_cli) {
        cli_process_char(key);
        return;
    }
    if (key == TERMINAL_CTRL_C) {
        cli_exit_remote();
    } else if (menu_active) {
        (void)input_routing_submit(INPUT_SOURCE_UART, input_menu_bind_terminal(key));
    } else {
        (void)term_inject_char(key);
    }
}

void input_routing_keyboard(const keyboard_batch_t* batch) {
    vet(batch != NULL && (batch->edge_count == 0 || batch->edges != NULL),
        "invalid keyboard batch");
    if (!menu_active) {
        keyboard_core_submit(batch);
        return;
    }
    for (size_t i = 0; i < batch->edge_count; ++i)
        if (batch->edges[i].pressed)
            (void)input_routing_submit(batch->source, input_menu_bind_hid(batch->edges[i].keycode));
}

void input_routing_gesture(input_source_t source, input_gesture_t gesture) {
    if (menu_active) {
        switch (gesture) {
            case input_gesture_button_short:
                (void)input_routing_submit(source, tui_action_down);
                break;
            case input_gesture_button_long:
                (void)input_routing_submit(source, tui_action_activate);
                break;
            case input_gesture_dual_shift:
                break;
        }
    } else {
        input_routing_request((input_system_request_t){source,
            gesture == input_gesture_button_short ? input_system_request_reset : input_system_request_menu});
    }
}

tui_action_t input_routing_navigation(void) {
    if (tail == head)
        return tui_action_none;
    const tui_action_t action = menu_queue[tail].action;
    tail = (tail + 1) & MENU_QUEUE_CAPACITY_MASK;
    return action;
}

void input_routing_request(input_system_request_t request) {
    if (request.action == input_system_none)
        return;
    // Simultaneous button/chord observations remain one machine request, not queued typing.
    if (system_request.action == input_system_none
        || (request_replaceable && request.source.device == INPUT_SOURCE_BUTTON.device
            && request.source.instance == INPUT_SOURCE_BUTTON.instance))
        system_request = request;
}

input_system_request_t input_routing_take_request(void) {
    const input_system_request_t request = system_request;
    system_request = (input_system_request_t){0};
    return request;
}

input_system_request_t input_routing_pending_request(void) {
    return system_request;
}
