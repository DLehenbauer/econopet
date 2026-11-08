// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "keyboard_test.h"

#include "input/input.h"
#include "input/input_controls.h"
#include "input/input_routing.h"
#include "input/input_uart.h"
#include "input/keyboard_core.h"
#include "input/term_inject.h"
#include "menu/menu_config.h"
#include "mock.h"
#include "usb/keyboard.h"
#include "usb/keyscan.h"

static system_state_t saved_state;
static bool expect_hid_output_report;

#define TEST_KEY_EVENT_QUEUE_CAPACITY 16

// Rejects unexpected HID output except for the explicit attachment test.
bool tuh_hid_set_report(uint8_t dev_addr, uint8_t idx, uint8_t report_id, uint8_t report_type,
                        void* report, uint16_t len) {
    (void) idx;
    (void) report_id;
    (void) report_type;
    (void) report;
    ck_assert_msg(expect_hid_output_report, "unexpected USB output report in keyboard input test");
    expect_hid_output_report = false;
    ck_assert(dev_addr == 1 || dev_addr == 2);
    ck_assert_uint_eq(len, sizeof(uint8_t));
    return true;
}

// Delivers a real HID report without dispatching the PET matrix or polling menu input.
static void report_key_from(uint8_t dev_addr, uint8_t instance, uint8_t keycode, uint8_t modifiers) {
    const hid_keyboard_report_t report = {.modifier = modifiers, .keycode = {keycode}};
    process_kbd_report(dev_addr, instance, &report);
}

static void report_key(uint8_t keycode, uint8_t modifiers) {
    report_key_from(1, 0, keycode, modifiers);
}

// Delivers a multi-key report without dispatching pending matrix events.
static void report_keys(uint8_t modifiers, const uint8_t* keycodes, size_t keycode_count) {
    hid_keyboard_report_t report = {.modifier = modifiers};
    ck_assert_msg(keycode_count <= count_of(report.keycode), "too many keys in test report");
    memcpy(report.keycode, keycodes, keycode_count);
    process_kbd_report(1, 0, &report);
}

// Drains modifier groups so assertions observe every queued report transition.
static void dispatch_all_key_events(void) {
    for (unsigned int i = 0; i < TEST_KEY_EVENT_QUEUE_CAPACITY; ++i)
        dispatch_key_events();
}

/** Restore mapping defaults through the router/core boundary without resetting machine RAM. */
static void reset_keyboard(system_state_t* state) {
    input_routing_reset();
    keyboard_core_reset(state);
    dispatch_key_events();
}

// Starts each report trace with an empty keyboard and firmware menu routing.
static void setup(void) {
    mock_reset();
    mock_clock_set(0);
    expect_hid_output_report = false;
    input_routing_set_menu_active(false);
    saved_state = system_state;
    system_state.terminal_session = terminal_session_cli;
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    report_key(0, 0);
    dispatch_all_key_events();
    reset_keyboard(&system_state);
    (void) mock_menu_requested();
    input_routing_set_menu_active(true);
}

// Releases any final press before restoring the shared machine configuration.
static void teardown(void) {
    report_key(0, 0);
    dispatch_key_events();
    reset_keyboard(&system_state);
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    (void) mock_menu_requested();
    input_routing_set_menu_active(false);
    system_state = saved_state;
}

static unsigned int menu_hid_input_index;
static const uint8_t menu_hid_keys[] = {HID_KEY_ARROW_DOWN, HID_KEY_ENTER};

// Supplies buffered HID navigation to the production configuration menu.
static tui_action_t menu_hid_input(void) {
    ck_assert_uint_lt(menu_hid_input_index, count_of(menu_hid_keys));
    report_key(menu_hid_keys[menu_hid_input_index++], 0);
    report_key(0, 0);
    return tui_action_none;
}

// Rapid taps survive batching, matrix dispatch and ring-buffer wraparound in order.
START_TEST(test_menu_batched_taps) {
    const uint8_t keys[] = {
        HID_KEY_ARROW_DOWN, HID_KEY_ARROW_DOWN, HID_KEY_ARROW_UP,
        HID_KEY_BACKSPACE, HID_KEY_ARROW_RIGHT, HID_KEY_ENTER
    };
    const int expected[] = {KEY_DOWN, KEY_DOWN, KEY_UP, '\b', KEY_RIGHT, '\r'};
    for (unsigned int batch = 0; batch < 4; batch++) {
        for (size_t i = 0; i < count_of(keys); i++) {
            report_key(keys[i], KEYBOARD_MODIFIER_RIGHTSHIFT);
            report_key(0, 0);
        }
        dispatch_all_key_events();
        for (size_t i = 0; i < count_of(expected); i++)
            ck_assert_int_eq(input_routing_navigation(), input_menu_bind_terminal(expected[i]));
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        for (size_t i = 0; i < count_of(usb_key_matrix); i++)
            ck_assert_uint_eq(usb_key_matrix[i], 0xff);
    }
}
END_TEST

// All control aliases are bound before emission, so source identity cannot reinterpret intent.
START_TEST(test_gesture_binding_table_and_priority) {
    const input_gesture_t gestures[] = {
        input_gesture_button_short, input_gesture_button_long, input_gesture_dual_shift,
    };
    const input_system_action_t runtime[] = {
        input_system_request_reset, input_system_request_menu, input_system_request_menu,
    };
    const tui_action_t menu[] = {tui_action_down, tui_action_activate, tui_action_none};
    const input_source_t keyboard = {3, 1};
    for (size_t i = 0; i < count_of(gestures); ++i) {
        const input_source_t source = i == 2 ? keyboard : INPUT_SOURCE_BUTTON;
        input_routing_set_menu_active(false);
        input_routing_gesture(source, gestures[i]);
        const input_system_request_t request = input_routing_take_request();
        ck_assert_int_eq(request.action, runtime[i]);
        ck_assert_uint_eq(request.source.device, source.device);
        ck_assert_uint_eq(request.source.instance, source.instance);
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        input_routing_set_menu_active(true);
        input_routing_gesture(source, gestures[i]);
        ck_assert_int_eq(input_routing_navigation(), menu[i]);
        ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
    }
    input_routing_set_menu_active(false);
    for (size_t button_first = 0; button_first < 2; ++button_first) {
        input_routing_begin_cycle();
        if (button_first)
            input_routing_gesture(INPUT_SOURCE_BUTTON, input_gesture_button_short);
        input_routing_gesture(keyboard, input_gesture_dual_shift);
        if (!button_first)
            input_routing_gesture(INPUT_SOURCE_BUTTON, input_gesture_button_short);
        input_routing_end_cycle();
        const input_system_request_t request = input_routing_take_request();
        ck_assert_int_eq(request.action, input_system_request_reset);
        ck_assert_uint_eq(request.source.device, INPUT_SOURCE_BUTTON.device);
        ck_assert_uint_eq(request.source.instance, INPUT_SOURCE_BUTTON.instance);
    }
}
END_TEST

// Queue reads neither observe a timed gesture nor consume its source hold.
START_TEST(test_queue_reads_do_not_poll_or_dispatch) {
    input_routing_set_menu_active(false);
    report_key(0, KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    for (size_t i = 0; i < 3; ++i) {
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
        ck_assert_int_eq(input_routing_take_request().action, input_system_none);
    }
    // Only observation emits intent. A delayed controller can leave it latched indefinitely.
    input_task();
    for (size_t i = 0; i < 3; ++i) {
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        ck_assert_int_eq(input_routing_pending_request().action, input_system_request_menu);
        mock_clock_advance(INPUT_LONG_PRESS_US);
    }
    ck_assert_int_eq(input_routing_take_request().action, input_system_request_menu);
    input_task();
    ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
}
END_TEST

// Batch edge order is transport-independent, with releases applied before press overrides.
START_TEST(test_core_batch_orders_edges_internally) {
    input_routing_set_menu_active(false);
    const input_source_t source = {3, 1};
    const usb_keymap_entry_t first = {.row = 2, .col = 3, .unshift = true};
    const usb_keymap_entry_t second = {.row = 3, .col = 3, .shift = true};
    const usb_keymap_entry_t shift = {.row = 0, .col = 3};
    const uint8_t modifiers = KEYBOARD_MODIFIER_LEFTSHIFT;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A | 0x100] = first;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_B | 0x100] = second;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_SHIFT_LEFT] = shift;
    keyboard_core_reset(&system_state);
    const keyboard_edge_t press = {HID_KEY_A, true};
    const keyboard_batch_t begin = {
        .source = source, .modifiers_pressed = modifiers, .edges = &press, .edge_count = 1,
    };
    keyboard_core_submit(&begin);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[first.col] & (1u << first.row), 0);
    ck_assert_uint_ne(usb_key_matrix[shift.col] & (1u << shift.row), 0);
    const keyboard_edge_t edges[] = {{HID_KEY_B, true}, {HID_KEY_A, false}};
    const keyboard_batch_t replace = {.source = source, .edges = edges, .edge_count = count_of(edges)};
    keyboard_core_submit(&replace);
    dispatch_all_key_events();
    ck_assert_uint_ne(usb_key_matrix[first.col] & (1u << first.row), 0);
    ck_assert_uint_eq(usb_key_matrix[second.col] & (1u << second.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 0);
    keyboard_core_cancel(source);
    dispatch_all_key_events();
    for (size_t i = 0; i < count_of(usb_key_matrix); ++i)
        ck_assert_uint_eq(usb_key_matrix[i], UINT8_MAX);
}
END_TEST

// Cancelling a leaf producer's pacing does not silently relinquish its runtime ownership.
START_TEST(test_terminal_leaf_cancellation) {
    input_routing_set_menu_active(false);
    const usb_keymap_entry_t cell = {.row = 2, .col = 3};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A] = cell;
    keyboard_core_reset(&system_state);
    ck_assert(term_inject_char('a'));
    input_task();
    term_inject_cancel();
    for (size_t i = 0; i < 4; ++i) input_task();
    ck_assert_uint_eq(usb_key_matrix[cell.col] & (1u << cell.row), 0);
    input_routing_cancel(INPUT_SOURCE_UART);
    input_task();
    ck_assert_uint_eq(usb_key_matrix[cell.col], UINT8_MAX);
}
END_TEST

// Backpressure dispatch can change locks, but LED transport still belongs to the USB adapter.
START_TEST(test_core_backpressure_retains_led_sync) {
    input_routing_set_menu_active(false);
    expect_hid_output_report = true;
    usb_keyboard_added(1, 0);
    ck_assert(!expect_hid_output_report);
    report_key(HID_KEY_CAPS_LOCK, 0);
    report_key(0, 0);
    for (unsigned int i = 0; i < TEST_KEY_EVENT_QUEUE_CAPACITY; ++i) {
        report_key(HID_KEY_A, 0);
        report_key(0, 0);
    }
    ck_assert(keyboard_core_caps_lock());
    expect_hid_output_report = true;
    dispatch_all_key_events();
    ck_assert(!expect_hid_output_report);
    usb_keyboard_removed(1, 0);
}
END_TEST

// Cancellation retires unpublished typing without discarding another producer's queued tap.
START_TEST(test_core_cancel_before_dispatch) {
    input_routing_set_menu_active(false);
    const input_source_t survivor = {4, 1};
    const uint8_t shift = KEYBOARD_MODIFIER_LEFTSHIFT;
    const uint8_t control = KEYBOARD_MODIFIER_RIGHTCTRL;
    const uint8_t cancelled_key = _i == 3 ? HID_KEY_CAPS_LOCK : HID_KEY_A;
    const usb_keymap_entry_t cancelled_cell = {.row = 2, .col = 3, .shift = _i == 1};
    const usb_keymap_entry_t survivor_cell = {.row = 3, .col = 3};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][cancelled_key] = cancelled_cell;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][cancelled_key | 0x100] = cancelled_cell;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_B] = survivor_cell;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_B | 0x100] = survivor_cell;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_SHIFT_LEFT]
        = (usb_keymap_entry_t){.row = 1, .col = 3};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_CONTROL_RIGHT]
        = (usb_keymap_entry_t){.row = 0, .col = 3};
    keyboard_core_reset(&system_state);
    const keyboard_edge_t cancelled_press = {cancelled_key, true};
    const uint8_t cancelled_modifiers = _i == 1 || _i == 3 ? 0 : shift;
    const keyboard_batch_t cancelled = {
        .source = INPUT_SOURCE_UART, .modifiers_pressed = cancelled_modifiers,
        .edges = &cancelled_press, .edge_count = 1,
    };
    keyboard_core_submit(&cancelled);
    if (_i == 2) {
        const keyboard_edge_t release = {cancelled_key, false};
        const keyboard_batch_t released = {
            .source = INPUT_SOURCE_UART, .modifiers_released = cancelled_modifiers,
            .edges = &release, .edge_count = 1,
        };
        keyboard_core_submit(&released);
    }
    const keyboard_edge_t press = {HID_KEY_B, true}, release = {HID_KEY_B, false};
    const keyboard_batch_t survivor_press = {
        .source = survivor, .modifiers_pressed = control, .edges = &press, .edge_count = 1,
    };
    const keyboard_batch_t survivor_release = {
        .source = survivor, .modifiers_released = control, .edges = &release, .edge_count = 1,
    };
    keyboard_core_submit(&survivor_press);
    keyboard_core_submit(&survivor_release);
    keyboard_core_cancel(INPUT_SOURCE_UART);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[survivor_cell.col], UINT8_MAX & ~(1u << survivor_cell.row) & ~1u);
    ck_assert(!keyboard_core_caps_lock());
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[survivor_cell.col], UINT8_MAX);
}
END_TEST

// Queued mapping locks affect later presses, whose resolved cells survive another map change.
START_TEST(test_queued_lock_changes_mapping) {
    input_routing_set_menu_active(false);
    system_state.pet_keyboard_model = pet_keyboard_model_graphics;
    const uint8_t lock = _i == 0 ? HID_KEY_SCROLL_LOCK : HID_KEY_NUM_LOCK;
    const pet_keyboard_model_t model = _i == 0
        ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    const usb_keymap_kind_t kind = _i == 0 ? usb_keymap_kind_positional : usb_keymap_kind_symbolic;
    const usb_keymap_entry_t original = {.row = 2, .col = 3};
    const usb_keymap_entry_t replacement = {.row = 3, .col = 4, .shift = true};
    const usb_keymap_entry_t shift = {.row = 1, .col = 3};
    system_state.usb_keymap_data[pet_keyboard_model_graphics][usb_keymap_kind_symbolic][HID_KEY_A] = original;
    system_state.usb_keymap_data[model][kind][HID_KEY_A] = replacement;
    system_state.usb_keymap_data[pet_keyboard_model_graphics][usb_keymap_kind_symbolic][HID_KEY_SHIFT_LEFT] = shift;
    system_state.usb_keymap_data[model][kind][HID_KEY_SHIFT_LEFT] = shift;
    keyboard_core_reset(&system_state);
    const keyboard_edge_t lock_down = {lock, true}, lock_up = {lock, false};
    const keyboard_edge_t down = {HID_KEY_A, true}, up = {HID_KEY_A, false};
    const keyboard_batch_t toggle_on = {.source = INPUT_SOURCE_UART, .edges = &lock_down, .edge_count = 1};
    const keyboard_batch_t toggle_off = {.source = INPUT_SOURCE_UART, .edges = &lock_up, .edge_count = 1};
    const keyboard_batch_t press = {.source = INPUT_SOURCE_UART, .edges = &down, .edge_count = 1};
    const keyboard_batch_t release = {.source = INPUT_SOURCE_UART, .edges = &up, .edge_count = 1};
    keyboard_core_submit(&toggle_on);
    keyboard_core_submit(&toggle_off);
    keyboard_core_submit(&press);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[replacement.col] & (1u << replacement.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col], UINT8_MAX & ~(1u << shift.row));
    keyboard_core_submit(&toggle_on);
    keyboard_core_submit(&toggle_off);
    keyboard_core_submit(&release);
    dispatch_all_key_events();
    for (size_t i = 0; i < count_of(usb_key_matrix); ++i)
        ck_assert_uint_eq(usb_key_matrix[i], UINT8_MAX);
}
END_TEST

// A lone Caps Lock edge updates Shift Lock immediately, including while input stays idle.
START_TEST(test_caps_lock_press_without_release) {
    input_routing_set_menu_active(false);
    const usb_keymap_entry_t shift = {.row = 1, .col = 3};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_SHIFT_LEFT] = shift;
    keyboard_core_reset(&system_state);
    expect_hid_output_report = true;
    usb_keyboard_added(1, 0);
    ck_assert(!expect_hid_output_report);
    const hid_keyboard_report_t press = {.keycode = {HID_KEY_CAPS_LOCK}}, release = {0};
    mock_input_hid_report(1, 0, &press);
    expect_hid_output_report = true;
    input_task();
    ck_assert(!expect_hid_output_report);
    ck_assert(keyboard_core_caps_lock());
    for (size_t i = 0; i < 3; ++i) {
        ck_assert_uint_eq(mock_input_published_matrix()[shift.col], UINT8_MAX & ~(1u << shift.row));
        input_task();
    }
    mock_input_hid_report(1, 0, &release);
    input_task();
    mock_input_hid_report(1, 0, &press);
    expect_hid_output_report = true;
    input_task();
    ck_assert(!expect_hid_output_report);
    ck_assert(!keyboard_core_caps_lock());
    ck_assert_uint_eq(mock_input_published_matrix()[shift.col], UINT8_MAX);
    usb_keyboard_removed(1, 0);
}
END_TEST

// Cancellation releases the published cell, not a remapped press still waiting in the queue.
START_TEST(test_cancel_uses_published_mapping) {
    input_routing_set_menu_active(false);
    const input_source_t source = {3, 1};
    const usb_keymap_entry_t original = {.row = 2, .col = 3};
    const usb_keymap_entry_t replacement = {.row = 3, .col = 4};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A] = original;
    keyboard_core_reset(&system_state);
    const keyboard_edge_t down = {HID_KEY_A, true}, up = {HID_KEY_A, false};
    const keyboard_batch_t press = {.source = source, .edges = &down, .edge_count = 1};
    const keyboard_batch_t release = {.source = source, .edges = &up, .edge_count = 1};
    keyboard_core_submit(&press);
    dispatch_all_key_events();
    keyboard_core_submit(&release);
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A] = replacement;
    keyboard_core_submit(&press);
    keyboard_core_cancel(source);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[original.col], UINT8_MAX);
    ck_assert_uint_eq(usb_key_matrix[replacement.col], UINT8_MAX);
}
END_TEST

// The shared core accepts source edges directly, without USB attachment or reports.
START_TEST(test_core_source_edges_without_reports) {
    input_routing_set_menu_active(false);
    const input_source_t first = {3, 0}, second = {4, 1};
    const uint8_t shift = KEYBOARD_MODIFIER_LEFTSHIFT;
    const usb_keymap_entry_t cell = {.row = 2, .col = 3};
    const usb_keymap_entry_t shift_cell = {.row = 0, .col = 3};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A] = cell;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A | 0x100] = cell;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_SHIFT_LEFT] = shift_cell;
    keyboard_core_reset(&system_state);
    const keyboard_edge_t edge = {HID_KEY_A, true};
    const keyboard_batch_t first_batch = {
        .source = first, .modifiers_pressed = shift, .edges = &edge, .edge_count = 1,
    };
    const keyboard_batch_t second_batch = {
        .source = second, .modifiers_pressed = shift, .edges = &edge, .edge_count = 1,
    };
    keyboard_core_submit(&first_batch);
    keyboard_core_submit(&second_batch);
    if (_i != 0) {
        dispatch_all_key_events();
        ck_assert_uint_eq(usb_key_matrix[cell.col] & (1u << cell.row), 0);
        ck_assert_uint_eq(usb_key_matrix[shift_cell.col] & (1u << shift_cell.row), 0);
    }

    // Cancelling one source preserves the other source's saved key and modifier mapping.
    keyboard_core_cancel(first);
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_SHIFT_LEFT]
        = (usb_keymap_entry_t){.row = 1, .col = 4};
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[cell.col] & (1u << cell.row), 0);
    keyboard_core_cancel(second);
    dispatch_all_key_events();
    for (size_t i = 0; i < count_of(usb_key_matrix); ++i)
        ck_assert_uint_eq(usb_key_matrix[i], UINT8_MAX);
}
END_TEST

// Machine intent survives release, input cancellation, and transitions without waiting for FIFO room.
START_TEST(test_typed_requests_are_independent_of_navigation) {
    const input_source_t keyboard = {1, 0};
    input_routing_set_menu_active(false);
    report_key(0, KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    report_key(0, 0);
    input_routing_set_menu_active(true);
    for (size_t i = 0; i < 15; ++i)
        ck_assert(input_routing_submit(INPUT_SOURCE_UART, tui_action_down));
    input_routing_cancel(keyboard);
    input_routing_set_menu_active(false);
    input_routing_set_menu_active(true);
    const input_system_request_t request = mock_take_request();
    ck_assert_int_eq(request.action, input_system_request_menu);
    ck_assert_uint_eq(request.source.device, keyboard.device);
    ck_assert_uint_eq(request.source.instance, keyboard.instance);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    ck_assert(mock_input_empty());
}
END_TEST

// A held navigation key and raw modifier cannot replay after two destination transitions.
START_TEST(test_router_suppresses_held_keys_and_modifiers) {
    const uint8_t shift = KEYBOARD_MODIFIER_LEFTSHIFT;
    const usb_keymap_entry_t key = {.row = 2, .col = 3};
    const usb_keymap_entry_t shift_key = {.row = 0, .col = 3};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_ARROW_DOWN] = key;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_ARROW_DOWN | 0x100] = key;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_SHIFT_LEFT] = shift_key;
    reset_keyboard(&system_state);
    input_routing_set_menu_active(false);
    report_key(HID_KEY_ARROW_DOWN, shift);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    input_routing_set_menu_active(true);
    report_key(HID_KEY_ARROW_DOWN, shift);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    input_routing_set_menu_active(false);
    report_key(HID_KEY_ARROW_DOWN, shift);
    dispatch_all_key_events();
    for (size_t i = 0; i < count_of(usb_key_matrix); ++i)
        ck_assert_uint_eq(usb_key_matrix[i], UINT8_MAX);
    report_key(0, 0);
    report_key(HID_KEY_ARROW_DOWN, shift);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift_key.col] & (1u << shift_key.row), 0);
}
END_TEST

// Keyboard disconnection cannot release a terminal key sharing its HID code.
START_TEST(test_disconnect_preserves_terminal_key) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const usb_keymap_entry_t key = {.row = 2, .col = 2};
    system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A] = key;
    reset_keyboard(&system_state);
    report_key_from(2, 0, HID_KEY_A, 0);
    input_uart_process('a');
    input_task();
    keyboard_source_removed(2, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    input_routing_cancel(INPUT_SOURCE_UART);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col], UINT8_MAX);
}
END_TEST

// Terminal menu aliases retain their meaning without passing through PET keymaps.
START_TEST(test_terminal_menu_aliases) {
    system_state.terminal_session = terminal_session_remote;
    input_uart_process('\r');
    input_uart_process('\n');
    input_uart_process(0x7f);
    input_uart_process('\e');
    ck_assert_int_eq(input_routing_navigation(), tui_action_activate);
    ck_assert_int_eq(input_routing_navigation(), tui_action_back);
    ck_assert_int_eq(input_routing_navigation(), tui_action_back);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
}
END_TEST

// Ordinary keys at the other PET layout's navigation positions are not menu commands.
START_TEST(test_physical_layout_isolation) {
    system_state.pet_keyboard_model = _i == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    const key_event_t foreign = _i == 0 ? PET_KEY_DOWN_B : PET_KEY_DOWN_N;
    pet_key_matrix[PET_KEY_COL(foreign)] &= ~(1u << PET_KEY_ROW(foreign));
    keyscan_sample();
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
}
END_TEST

// Remote CRLF produces one paced Enter, and reset discards pending terminal input.
START_TEST(test_runtime_crlf_and_reset) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const usb_keymap_entry_t enter = {.row = 5, .col = 6};
    system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_ENTER] = enter;
    reset_keyboard(&system_state);
    input_uart_process('\r');
    input_uart_process('\n');
    ck_assert_uint_eq(mock_term_inject_count(), 1);
    input_task();
    ck_assert_uint_eq(usb_key_matrix[enter.col] & (1u << enter.row), 0);
    input_task();
    input_task();
    ck_assert_uint_eq(usb_key_matrix[enter.col], UINT8_MAX);
    for (int i = 0; i < 6; ++i) input_task();
    ck_assert_uint_eq(usb_key_matrix[enter.col], UINT8_MAX);
    input_uart_process('\n');
    ck_assert_uint_eq(mock_term_inject_count(), 2);
    reset_keyboard(&system_state);
    for (int i = 0; i < 6; ++i) input_task();
    ck_assert_uint_eq(usb_key_matrix[enter.col], UINT8_MAX);
}
END_TEST

// Overflow never invents extra runtime presses and cancellation frees the queue.
START_TEST(test_runtime_injection_capacity) {
    input_routing_set_menu_active(false);
    for (int i = 0; i < 15; ++i)
        ck_assert(term_inject_char('a'));
    ck_assert(!term_inject_char('a'));
    input_routing_cancel(INPUT_SOURCE_UART);
    ck_assert(term_inject_char('a'));
    input_routing_cancel(INPUT_SOURCE_UART);
}
END_TEST

// Native sources share one FIFO, with physical snapshots ordered when observed.
START_TEST(test_shared_menu_order) {
    system_state.pet_keyboard_model = _i == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    system_state.terminal_session = terminal_session_remote;
    report_key(HID_KEY_ARROW_DOWN, 0);
    input_uart_process(KEY_LEFT);
    const key_event_t key = _i == 0 ? PET_KEY_RETURN_N : PET_KEY_RETURN_B;
    pet_key_matrix[PET_KEY_COL(key)] &= ~(1u << PET_KEY_ROW(key));
    keyscan_sample();
    report_key(0, 0);
    input_uart_process(KEY_UP);
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_page_up);
    ck_assert_int_eq(input_routing_navigation(), tui_action_activate);
    ck_assert_int_eq(input_routing_navigation(), tui_action_up);
    ck_assert(mock_input_empty());
}
END_TEST

// Full queues reject presses explicitly and retain ordering across wrap and cancellation.
START_TEST(test_shared_menu_capacity) {
    for (int i = 0; i < 15; ++i)
        ck_assert(input_routing_submit(INPUT_SOURCE_UART, tui_action_up));
    ck_assert(!input_routing_submit(INPUT_SOURCE_PET, tui_action_down));
    for (int i = 0; i < 7; ++i)
        ck_assert_int_eq(input_routing_navigation(), tui_action_up);
    for (int i = 0; i < 7; ++i)
        ck_assert(input_routing_submit(INPUT_SOURCE_PET, tui_action_down));
    input_routing_cancel(INPUT_SOURCE_UART);
    for (int i = 0; i < 7; ++i)
        ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);

    // Even while navigation is full, control events retain priority and consumption.
    for (int i = 0; i < 15; ++i)
        ck_assert(input_routing_submit(INPUT_SOURCE_UART, tui_action_up));
    input_routing_request((input_system_request_t){INPUT_SOURCE_PET, input_system_request_menu});
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    ck_assert_int_eq(input_routing_navigation(), tui_action_up);
}
END_TEST

// New menu typing never alters the injected matrix, and held keys do not replay on exit.
START_TEST(test_menu_typing_and_transition) {
    system_state.terminal_session = terminal_session_remote;
    report_key(HID_KEY_T, KEYBOARD_MODIFIER_LEFTSHIFT);
    input_uart_process('t');
    dispatch_all_key_events();
    for (size_t i = 0; i < KEY_COL_COUNT; ++i)
        ck_assert_uint_eq(usb_key_matrix[i], UINT8_MAX);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    input_routing_set_menu_active(false);
    report_key(HID_KEY_T, KEYBOARD_MODIFIER_LEFTSHIFT);
    dispatch_all_key_events();
    for (size_t i = 0; i < KEY_COL_COUNT; ++i)
        ck_assert_uint_eq(usb_key_matrix[i], UINT8_MAX);
    report_key(0, 0);
}
END_TEST

// Terminal releases and USB releases cannot retire another producer's same HID key.
START_TEST(test_terminal_usb_ownership) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const usb_keymap_entry_t key = {.row = 2, .col = 2};
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    keymap[HID_KEY_A] = key;
    reset_keyboard(&system_state);

    if (_i == 0) {
        report_key(HID_KEY_A, 0);
        input_uart_process('a');
        input_task();
        input_routing_cancel(INPUT_SOURCE_UART);
    } else {
        input_uart_process('a');
        input_task();
        report_key(HID_KEY_A, 0);
        report_key(0, 0);
    }
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    if (_i == 0) report_key(0, 0);
    else input_routing_cancel(INPUT_SOURCE_UART);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col], UINT8_MAX);
}
END_TEST

// Different HID codes mapped to one cell preserve each other's ownership.
START_TEST(test_terminal_mapped_cell_ownership) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const usb_keymap_entry_t key = {.row = 2, .col = 2};
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    keymap[HID_KEY_A] = keymap[HID_KEY_B] = key;
    reset_keyboard(&system_state);
    report_key(HID_KEY_B, 0);
    input_uart_process('a');
    input_task();
    input_routing_cancel(INPUT_SOURCE_UART);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    report_key(0, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col], UINT8_MAX);
}
END_TEST

// Mapping changes during a hold cannot redirect its eventual release.
START_TEST(test_release_uses_saved_mapping) {
    input_routing_set_menu_active(false);
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    keymap[HID_KEY_A] = (usb_keymap_entry_t){.row = 2, .col = 2};
    reset_keyboard(&system_state);
    report_key(HID_KEY_A, 0);
    dispatch_all_key_events();
    keymap[HID_KEY_A] = (usb_keymap_entry_t){.row = 3, .col = 3};
    report_key(0, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[2], UINT8_MAX);
    ck_assert_uint_eq(usb_key_matrix[3], UINT8_MAX);
}
END_TEST

// Context changes cancel queued and active terminal presses and baseline physical holds.
START_TEST(test_cancel_runtime_injection) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    input_uart_process('a');
    input_uart_process('b');
    input_task();
    const key_event_t physical = PET_KEY_DOWN_N;
    pet_key_matrix[PET_KEY_COL(physical)] &= ~(1u << PET_KEY_ROW(physical));
    input_routing_set_menu_active(true);
    for (int i = 0; i < 8; ++i)
        input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    for (size_t i = 0; i < KEY_COL_COUNT; ++i)
        ck_assert_uint_eq(usb_key_matrix[i], UINT8_MAX);
    pet_key_matrix[PET_KEY_COL(physical)] |= 1u << PET_KEY_ROW(physical);
    keyscan_sample();
    pet_key_matrix[PET_KEY_COL(physical)] &= ~(1u << PET_KEY_ROW(physical));
    keyscan_sample();
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
}
END_TEST

// Independent synthetic Shift cannot release a USB Shift or qualify a raw dual-Shift gesture.
START_TEST(test_terminal_modifier_ownership) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    const usb_keymap_entry_t left = {.row = 0, .col = 8};
    const usb_keymap_entry_t right = {.row = 5, .col = 8};
    keymap[HID_KEY_SHIFT_LEFT] = left;
    keymap[HID_KEY_SHIFT_RIGHT] = right;
    keymap[HID_KEY_A | 0x100] = (usb_keymap_entry_t){.row = 2, .col = 2};
    reset_keyboard(&system_state);
    report_key(0, KEYBOARD_MODIFIER_RIGHTSHIFT);
    input_uart_process('A');
    input_task();
    dispatch_all_key_events();
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!mock_menu_requested());
    ck_assert_uint_eq(usb_key_matrix[left.col] & (1u << left.row), 0);
    ck_assert_uint_eq(usb_key_matrix[right.col] & (1u << right.row), 0);
    input_routing_cancel(INPUT_SOURCE_UART);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[left.col] & (1u << left.row), 1u << left.row);
    ck_assert_uint_eq(usb_key_matrix[right.col] & (1u << right.row), 0);
    report_key(0, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[right.col], UINT8_MAX);
}
END_TEST

// Unchanged held-key reports are not repeated, but release/repress creates another press.
START_TEST(test_menu_held_key_reports) {
    report_key(HID_KEY_ARROW_DOWN, 0);
    report_key(HID_KEY_ARROW_DOWN, 0);
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    report_key(0, 0);
    report_key(HID_KEY_ARROW_DOWN, 0);
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
}
END_TEST

// Menu aliases use HID semantics rather than whichever PET keymap was loaded.
START_TEST(test_menu_navigation_keys) {
    const uint8_t keys[] = {
        HID_KEY_ARROW_LEFT, HID_KEY_PAGE_UP, HID_KEY_PAGE_DOWN, HID_KEY_KEYPAD_ENTER,
        HID_KEY_ESCAPE, HID_KEY_BACKSPACE, HID_KEY_DELETE, HID_KEY_INSERT
    };
    const int expected[] = {
        KEY_LEFT, KEY_PGUP, KEY_PGDN, '\r', '\e', '\b', EOF, EOF
    };
    for (size_t i = 0; i < count_of(keys); i++) {
        report_key(keys[i], 0);
        report_key(0, 0);
        ck_assert_int_eq(input_routing_navigation(), input_menu_bind_terminal(expected[i]));
    }
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
}
END_TEST

// Navigation outside the menu still reaches the configured PET keyboard matrix.
START_TEST(test_runtime_navigation_uses_pet_matrix) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const key_event_t down = PET_KEY_DOWN_N;
    usb_keymap_entry_t* const entry = &system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_ARROW_DOWN];
    *entry = (usb_keymap_entry_t) {.row = PET_KEY_ROW(down), .col = PET_KEY_COL(down)};
    reset_keyboard(&system_state);
    report_key(HID_KEY_ARROW_DOWN, 0);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[PET_KEY_COL(down)], 0xff & ~(1u << PET_KEY_ROW(down)));
    if (_i != 0) system_state.terminal_session = terminal_session_cli;
    report_key(0, 0);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[PET_KEY_COL(down)], 0xff);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
}
END_TEST

// Reset and leaving menu routing discard unread presses instead of replaying them later.
START_TEST(test_menu_queue_lifecycle) {
    report_key(HID_KEY_ENTER, 0);
    report_key(0, 0);
    if (_i == 0) reset_keyboard(&system_state);
    else {
        input_routing_set_menu_active(false);
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        input_routing_set_menu_active(true);
    }
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
}
END_TEST

// Only a continuous raw dual Shift hold qualifies, with release required after consumption.
START_TEST(test_usb_menu_chord) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, KEYBOARD_MODIFIER_LEFTSHIFT);
    ck_assert(!mock_menu_requested());
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert(!mock_menu_requested());
    mock_clock_advance(1);
    ck_assert(mock_menu_requested());
    mock_consume_requests();
    ck_assert(!mock_menu_requested());
    report_key(0, both);
    ck_assert(!mock_menu_requested());
    reset_keyboard(&system_state);
    report_key(0, both);
    ck_assert(!mock_menu_requested());
    report_key(0, KEYBOARD_MODIFIER_RIGHTSHIFT);
    report_key(0, both);
    report_key(0, 0);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!mock_menu_requested());

    // Routing does not change recognition. Dispatch can ignore the consumed event.
    system_state.terminal_session = terminal_session_cli;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    system_state.terminal_session = terminal_session_remote;
    ck_assert(!mock_menu_requested());
    report_key(0, both);
    ck_assert(!mock_menu_requested());
}
END_TEST

// A qualified USB chord released before polling remains a pending menu event.
START_TEST(test_usb_menu_chord_release_before_poll) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    report_key(0, 0);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    ck_assert(mock_input_empty());
}
END_TEST

// Runtime cancellation cannot revoke machine intent emitted before the transition.
START_TEST(test_usb_menu_chord_reset_before_poll) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    report_key(0, 0);
    reset_keyboard(&system_state);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    ck_assert(mock_input_empty());
}
END_TEST

// Removing a source cancels typing, but cannot revoke its emitted machine intent.
START_TEST(test_usb_menu_chord_removed_before_poll) {
    input_routing_set_menu_active(false);
    expect_hid_output_report = true;
    usb_keyboard_added(1, 0);
    ck_assert(!expect_hid_output_report);

    system_state.terminal_session = terminal_session_remote;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    report_key(0, 0);
    usb_keyboard_removed(1, 0);
    const input_system_request_t request = mock_take_request();
    ck_assert_int_eq(request.action, input_system_request_menu);
    ck_assert_uint_eq(request.source.device, 1);
    ck_assert_uint_eq(request.source.instance, 0);
    ck_assert(mock_input_empty());
}
END_TEST

// Reports and removal from one keyboard do not cancel another keyboard's chord.
START_TEST(test_usb_menu_chords_are_per_keyboard) {
    input_routing_set_menu_active(false);
    expect_hid_output_report = true;
    usb_keyboard_added(1, 0);
    ck_assert(!expect_hid_output_report);
    expect_hid_output_report = true;
    usb_keyboard_added(2, 0);
    ck_assert(!expect_hid_output_report);

    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key_from(1, 0, 0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US / 2);
    report_key_from(2, 0, 0, 0);
    mock_clock_advance(INPUT_LONG_PRESS_US / 2);
    ck_assert(mock_menu_requested());

    usb_keyboard_removed(2, 0);
    ck_assert(mock_menu_requested());
    mock_consume_requests();
    usb_keyboard_removed(1, 0);
}
END_TEST

// Reports and removal from one HID interface do not cancel another interface's chord.
START_TEST(test_usb_menu_chords_are_per_interface) {
    input_routing_set_menu_active(false);
    expect_hid_output_report = true;
    usb_keyboard_added(1, 0);
    ck_assert(!expect_hid_output_report);
    expect_hid_output_report = true;
    usb_keyboard_added(1, 1);
    ck_assert(!expect_hid_output_report);

    const usb_keymap_entry_t key = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A];
    report_key_from(1, 0, HID_KEY_A, 0);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    report_key_from(1, 1, HID_KEY_A, 0);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    report_key_from(1, 0, 0, 0);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    report_key_from(1, 1, 0, 0);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 1u << key.row);

    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key_from(1, 0, 0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US / 2);
    report_key_from(1, 1, 0, 0);
    mock_clock_advance(INPUT_LONG_PRESS_US / 2);
    ck_assert(mock_menu_requested());

    usb_keyboard_removed(1, 1);
    ck_assert(mock_menu_requested());
    mock_consume_requests();
    usb_keyboard_removed(1, 0);
}
END_TEST

// Modifier-only release and disconnect both release injected PET matrix keys.
START_TEST(test_usb_keyboard_disconnect_releases_keys_and_modifiers) {
    input_routing_set_menu_active(false);
    expect_hid_output_report = true;
    usb_keyboard_added(1, 0);
    ck_assert(!expect_hid_output_report);

    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    const usb_keymap_entry_t key = {.row = 0, .col = 0};
    const usb_keymap_entry_t shift = {.row = 1, .col = 0};
    keymap[HID_KEY_A] = key;
    keymap[HID_KEY_A | 0x100] = key;
    keymap[HID_KEY_SHIFT_LEFT] = shift;
    const uint8_t left_shift = KEYBOARD_MODIFIER_LEFTSHIFT;

    report_key_from(1, 0, HID_KEY_A, left_shift);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 0);

    report_key_from(1, 0, 0, left_shift);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 1u << key.row);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 0);

    report_key_from(1, 0, 0, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 1u << shift.row);

    report_key_from(1, 0, HID_KEY_A, left_shift);
    dispatch_all_key_events();
    usb_keyboard_removed(1, 0);
    dispatch_all_key_events();
    for (size_t i = 0; i < count_of(usb_key_matrix); ++i)
        ck_assert_uint_eq(usb_key_matrix[i], 0xff);
    ck_assert(!mock_menu_requested());
}
END_TEST

// Removing one interface discards only its unread navigation events.
START_TEST(test_menu_queue_removal_preserves_other_sources) {
    expect_hid_output_report = true;
    usb_keyboard_added(1, 0);
    ck_assert(!expect_hid_output_report);
    expect_hid_output_report = true;
    usb_keyboard_added(2, 0);
    ck_assert(!expect_hid_output_report);

    report_key_from(1, 0, HID_KEY_ARROW_DOWN, 0);
    report_key_from(2, 0, HID_KEY_ENTER, 0);
    usb_keyboard_removed(2, 0);
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    usb_keyboard_removed(1, 0);
}
END_TEST

// Identical held navigation keys still produce one menu press edge per interface.
START_TEST(test_menu_press_edges_are_per_interface) {
    report_key_from(1, 0, HID_KEY_ARROW_DOWN, 0);
    report_key_from(2, 0, HID_KEY_ARROW_DOWN, 0);
    report_key_from(1, 0, 0, 0);
    report_key_from(2, 0, 0, 0);

    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    keyboard_source_removed(2, 0);
}
END_TEST

// Menu-only holds neither suppress runtime presses nor retain their releases.
START_TEST(test_menu_held_key_does_not_own_runtime_key) {
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    const usb_keymap_entry_t down = {.row = 0, .col = 0};
    keymap[HID_KEY_ARROW_DOWN] = down;
    reset_keyboard(&system_state);

    report_key_from(1, 0, HID_KEY_ARROW_DOWN, 0);
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    input_routing_set_menu_active(false);
    report_key_from(2, 0, HID_KEY_ARROW_DOWN, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[down.col] & (1u << down.row), 0);

    if (_i == 0) report_key_from(2, 0, 0, 0);
    else keyboard_source_removed(2, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[down.col] & (1u << down.row), 1u << down.row);

    report_key_from(2, 0, HID_KEY_ARROW_DOWN, 0);
    dispatch_all_key_events();
    if (_i == 0) report_key_from(1, 0, 0, 0);
    else keyboard_source_removed(1, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[down.col] & (1u << down.row), 0);
    report_key_from(2, 0, 0, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[down.col] & (1u << down.row), 1u << down.row);
    keyboard_source_removed(2, 0);
}
END_TEST

// PET business and graphics keyboards require their own two physical Shift keys.
START_TEST(test_pet_menu_chord) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    system_state.pet_keyboard_model = _i == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    const key_event_t left = _i == 0 ? PET_KEY_LSHIFT_N : PET_KEY_LSHIFT_B;
    const key_event_t right = _i == 0 ? PET_KEY_RSHIFT_N : PET_KEY_RSHIFT_B;
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(right)] &= ~(1u << PET_KEY_ROW(right));
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_menu_requested());
    mock_consume_requests();
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(left)] |= 1u << PET_KEY_ROW(left);
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_menu_requested());
    mock_consume_requests();

    // Typing before the deadline blocks the chord until either Shift is released.
    const key_event_t typing = _i == 0 ? PET_KEY_T_N : PET_KEY_T_B;
    pet_key_matrix[PET_KEY_COL(left)] |= 1u << PET_KEY_ROW(left);
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    pet_key_matrix[PET_KEY_COL(typing)] &= ~(1u << PET_KEY_ROW(typing));
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(typing)] |= 1u << PET_KEY_ROW(typing);
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US * 2);
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(right)] |= 1u << PET_KEY_ROW(right);
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(right)] &= ~(1u << PET_KEY_ROW(right));
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_menu_requested());

    // Menu dispatch can ignore the physical chord without replaying it on exit.
    system_state.terminal_session = terminal_session_cli;
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    ck_assert(!mock_menu_requested());
    system_state.terminal_session = terminal_session_remote;
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!mock_menu_requested());
}
END_TEST

// A key already down when the PET chord starts also blocks it in both layouts.
START_TEST(test_pet_menu_chord_with_key_already_down) {
    input_routing_set_menu_active(false);
    system_state.pet_keyboard_model = _i == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    const key_event_t left = _i == 0 ? PET_KEY_LSHIFT_N : PET_KEY_LSHIFT_B;
    const key_event_t right = _i == 0 ? PET_KEY_RSHIFT_N : PET_KEY_RSHIFT_B;
    const key_event_t typing = _i == 0 ? PET_KEY_T_N : PET_KEY_T_B;
    pet_key_matrix[PET_KEY_COL(typing)] &= ~(1u << PET_KEY_ROW(typing));
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    pet_key_matrix[PET_KEY_COL(right)] &= ~(1u << PET_KEY_ROW(right));
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(typing)] |= 1u << PET_KEY_ROW(typing);
    mock_clock_advance(INPUT_LONG_PRESS_US * 2);
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(left)] |= 1u << PET_KEY_ROW(left);
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_menu_requested());
}
END_TEST

// Both PET layouts retain a qualified chord when release precedes the next poll.
START_TEST(test_pet_menu_chord_release_before_poll) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    system_state.pet_keyboard_model = _i == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    const key_event_t left = _i == 0 ? PET_KEY_LSHIFT_N : PET_KEY_LSHIFT_B;
    const key_event_t right = _i == 0 ? PET_KEY_RSHIFT_N : PET_KEY_RSHIFT_B;
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    pet_key_matrix[PET_KEY_COL(right)] &= ~(1u << PET_KEY_ROW(right));
    ck_assert(!mock_menu_requested());

    mock_clock_advance(INPUT_LONG_PRESS_US);
    pet_key_matrix[PET_KEY_COL(left)] |= 1u << PET_KEY_ROW(left);
    pet_key_matrix[PET_KEY_COL(right)] |= 1u << PET_KEY_ROW(right);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    ck_assert(mock_input_empty());
}
END_TEST

// A PET typing key observed after qualification does not erase the pending event.
START_TEST(test_pet_menu_chord_typing_after_deadline) {
    input_routing_set_menu_active(false);
    system_state.pet_keyboard_model = _i == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    const key_event_t left = _i == 0 ? PET_KEY_LSHIFT_N : PET_KEY_LSHIFT_B;
    const key_event_t right = _i == 0 ? PET_KEY_RSHIFT_N : PET_KEY_RSHIFT_B;
    const key_event_t typing = _i == 0 ? PET_KEY_T_N : PET_KEY_T_B;
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    pet_key_matrix[PET_KEY_COL(right)] &= ~(1u << PET_KEY_ROW(right));
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    pet_key_matrix[PET_KEY_COL(typing)] &= ~(1u << PET_KEY_ROW(typing));
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    pet_key_matrix[PET_KEY_COL(typing)] |= 1u << PET_KEY_ROW(typing);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_input_empty());
}
END_TEST

// A keymap can inject left Shift for right-Shift-A. That is not a two-Shift chord.
START_TEST(test_implicit_shift_does_not_open_menu) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    system_state.pet_keyboard_model = pet_keyboard_model_graphics;
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        pet_keyboard_model_graphics][usb_keymap_kind_symbolic];
    keymap[HID_KEY_A | 0x100] = (usb_keymap_entry_t) {.row = 0, .col = 0, .shift = true};
    keymap[HID_KEY_SHIFT_LEFT] = (usb_keymap_entry_t) {
        .row = PET_KEY_ROW(PET_KEY_LSHIFT_N), .col = PET_KEY_COL(PET_KEY_LSHIFT_N)
    };
    keymap[HID_KEY_SHIFT_RIGHT] = (usb_keymap_entry_t) {
        .row = PET_KEY_ROW(PET_KEY_RSHIFT_N), .col = PET_KEY_COL(PET_KEY_RSHIFT_N)
    };
    reset_keyboard(&system_state);
    report_key(HID_KEY_A, KEYBOARD_MODIFIER_RIGHTSHIFT);
    dispatch_all_key_events();
    memcpy(pet_key_matrix, usb_key_matrix, sizeof(pet_key_matrix));
    ck_assert(!mock_menu_requested());
    report_key(0, 0);
    dispatch_all_key_events();
    ck_assert(!mock_menu_requested());
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    ck_assert(!mock_menu_requested());
    pet_key_matrix[PET_KEY_COL(PET_KEY_LSHIFT_N)] &= ~(1u << PET_KEY_ROW(PET_KEY_LSHIFT_N));
    pet_key_matrix[PET_KEY_COL(PET_KEY_RSHIFT_N)] &= ~(1u << PET_KEY_ROW(PET_KEY_RSHIFT_N));
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_menu_requested());
}
END_TEST

// Batched shifted reports cannot overflow the queue and lose key releases.
START_TEST(test_batched_shifted_reports_preserve_key_releases) {
    input_routing_set_menu_active(false);
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    const usb_keymap_entry_t a = {.row = 0, .col = 0};
    const usb_keymap_entry_t shifted_a = {.row = 1, .col = 0};
    const usb_keymap_entry_t b = {.row = 2, .col = 0};
    const usb_keymap_entry_t shifted_b = {.row = 3, .col = 0};
    const usb_keymap_entry_t shift = {.row = 4, .col = 0};
    keymap[HID_KEY_A] = a;
    keymap[HID_KEY_A | 0x100] = shifted_a;
    keymap[HID_KEY_B] = b;
    keymap[HID_KEY_B | 0x100] = shifted_b;
    keymap[HID_KEY_SHIFT_LEFT] = shift;
    reset_keyboard(&system_state);

    const uint8_t left_shift = KEYBOARD_MODIFIER_LEFTSHIFT;
    const uint8_t held_a[] = {HID_KEY_A};
    const uint8_t held_a_and_b[] = {HID_KEY_A, HID_KEY_B};
    const uint8_t released[] = {0};
    report_keys(left_shift, held_a, count_of(held_a));
    for (unsigned int tap = 0; tap < 6; ++tap) {
        report_keys(left_shift, held_a_and_b, count_of(held_a_and_b));
        report_keys(left_shift, held_a, count_of(held_a));
    }

    report_keys(left_shift, held_a_and_b, count_of(held_a_and_b));
    report_keys(left_shift, held_a, count_of(held_a));
    report_keys(left_shift, released, count_of(released));
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[shifted_a.col] & (1u << shifted_a.row), 1u << shifted_a.row);
    ck_assert_uint_eq(usb_key_matrix[shifted_b.col] & (1u << shifted_b.row), 1u << shifted_b.row);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 0);

    report_keys(0, released, count_of(released));
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[shifted_a.col] & (1u << shifted_a.row), 1u << shifted_a.row);
    ck_assert_uint_eq(usb_key_matrix[shifted_b.col] & (1u << shifted_b.row), 1u << shifted_b.row);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 1u << shift.row);
}
END_TEST

// A simultaneous Shift and unshifted key must not expose Shift on a held key first.
START_TEST(test_unshift_mapping_precedes_simultaneous_modifier) {
    input_routing_set_menu_active(false);
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    const usb_keymap_entry_t a = {.row = 0, .col = 0, .unshift = true};
    const usb_keymap_entry_t b = {.row = 1, .col = 0};
    const usb_keymap_entry_t shift = {.row = 2, .col = 0};
    keymap[HID_KEY_A | 0x100] = a;
    keymap[HID_KEY_B] = b;
    keymap[HID_KEY_SHIFT_LEFT] = shift;
    reset_keyboard(&system_state);

    const uint8_t held_b[] = {HID_KEY_B};
    const uint8_t held_a_and_b[] = {
        _i == 0 ? HID_KEY_A : HID_KEY_B, _i == 0 ? HID_KEY_B : HID_KEY_A
    };
    report_keys(0, held_b, count_of(held_b));
    dispatch_all_key_events();
    report_keys(KEYBOARD_MODIFIER_LEFTSHIFT, held_a_and_b, count_of(held_a_and_b));
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[b.col] & (1u << b.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 1u << shift.row);
    ck_assert_uint_eq(usb_key_matrix[a.col] & (1u << a.row), 0);

    report_key(0, 0);
    dispatch_all_key_events();
}
END_TEST

// Repeated reports preserve keymap Shift overrides while a key remains held.
START_TEST(test_keymap_shift_overrides_survive_unchanged_reports) {
    input_routing_set_menu_active(false);
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    const usb_keymap_entry_t key = {.row = 0, .col = 0};
    const usb_keymap_entry_t shift = {.row = 1, .col = 0};
    const usb_keymap_entry_t control = {.row = 2, .col = 0};
    keymap[HID_KEY_A] = (usb_keymap_entry_t) {.row = key.row, .col = key.col, .shift = true};
    keymap[HID_KEY_A | 0x100] =
        (usb_keymap_entry_t) {.row = key.row, .col = key.col, .unshift = true};
    keymap[HID_KEY_SHIFT_LEFT] = shift;
    keymap[HID_KEY_CONTROL_LEFT] = control;
    reset_keyboard(&system_state);

    report_key(HID_KEY_A, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 0);
    report_key(HID_KEY_A, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 0);
    const uint8_t left_control = KEYBOARD_MODIFIER_LEFTCTRL;
    report_key(HID_KEY_A, left_control);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 0);
    ck_assert_uint_eq(usb_key_matrix[control.col] & (1u << control.row), 0);
    report_key(0, left_control);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 1u << key.row);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 1u << shift.row);
    ck_assert_uint_eq(usb_key_matrix[control.col] & (1u << control.row), 0);
    report_key(0, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[control.col] & (1u << control.row), 1u << control.row);

    const uint8_t left_shift = KEYBOARD_MODIFIER_LEFTSHIFT;
    report_key(HID_KEY_A, left_shift);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 1u << shift.row);
    report_key(HID_KEY_A, left_shift);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 1u << shift.row);
    const uint8_t left_shift_and_control = left_shift | left_control;
    report_key(HID_KEY_A, left_shift_and_control);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 0);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 1u << shift.row);
    ck_assert_uint_eq(usb_key_matrix[control.col] & (1u << control.row), 0);
    report_key(0, left_shift_and_control);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[key.col] & (1u << key.row), 1u << key.row);
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 0);
    ck_assert_uint_eq(usb_key_matrix[control.col] & (1u << control.row), 0);
    report_key(0, 0);
    dispatch_all_key_events();
    ck_assert_uint_eq(usb_key_matrix[shift.col] & (1u << shift.row), 1u << shift.row);
    ck_assert_uint_eq(usb_key_matrix[control.col] & (1u << control.row), 1u << control.row);
}
END_TEST

// Typing before the deadline blocks the chord through slow typing until a Shift is released.
START_TEST(test_typing_blocks_menu_hold) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    report_key(HID_KEY_A, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!mock_menu_requested());
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US * 2);
    ck_assert(!mock_menu_requested());
    report_key(HID_KEY_A, both);
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!mock_menu_requested());
    reset_keyboard(&system_state);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!mock_menu_requested());
    report_key(0, KEYBOARD_MODIFIER_RIGHTSHIFT);
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert(!mock_menu_requested());
    mock_clock_advance(1);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    ck_assert(mock_input_empty());
}
END_TEST

// A held key or extra modifier blocks a chord, including when it starts.
START_TEST(test_usb_menu_chord_with_other_key) {
    input_routing_set_menu_active(false);
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    const uint8_t extra = KEYBOARD_MODIFIER_LEFTCTRL;
    if (_i == 0) {
        report_key(HID_KEY_A, 0);
        report_key(HID_KEY_A, both);
    } else if (_i == 1) {
        report_key(0, both | extra);
    } else {
        report_key(0, both);
        mock_clock_advance(INPUT_LONG_PRESS_US - 1);
        report_key(0, both | extra);
    }
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US * 2);
    ck_assert(!mock_menu_requested());
    report_key(0, KEYBOARD_MODIFIER_LEFTSHIFT);
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_menu_requested());
}
END_TEST

// A qualified chord remains pending when typing begins before menu polling.
START_TEST(test_usb_menu_chord_typing_after_deadline) {
    input_routing_set_menu_active(false);
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    report_key(HID_KEY_A, both);
    report_key(0, both);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_input_empty());
}
END_TEST

// A blocked interface does not block a different one and removal discards its block.
START_TEST(test_usb_menu_block_is_per_interface) {
    input_routing_set_menu_active(false);
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key_from(1, 0, HID_KEY_A, both);
    report_key_from(1, 0, 0, both);
    report_key_from(1, 1, 0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_menu_requested());
    keyboard_source_removed(1, 1);
    const input_system_request_t request = mock_take_request();
    ck_assert_int_eq(request.action, input_system_request_menu);
    ck_assert_uint_eq(request.source.instance, 1);
    ck_assert(!mock_menu_requested());
    keyboard_source_removed(1, 0);
    report_key_from(1, 0, 0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_menu_requested());
}
END_TEST

// USB release/repress within a servicing batch cannot inherit the previous hold.
START_TEST(test_batched_menu_release_restarts_hold) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    report_key(0, 0);
    report_key(0, both);
    mock_clock_advance(1);
    ck_assert(!mock_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert(mock_menu_requested());
}
END_TEST

static size_t legacy_menu_key_index;
static const input_gesture_t menu_gestures[] = {
    input_gesture_dual_shift, input_gesture_button_short,
    input_gesture_dual_shift, input_gesture_button_long,
};

// Supplies a bounded trace so an ignored activation cannot hang the menu test.
static tui_action_t legacy_menu_input(void) {
    ck_assert_uint_lt(legacy_menu_key_index, count_of(menu_gestures));
    const input_gesture_t gesture = menu_gestures[legacy_menu_key_index++];
    input_routing_gesture(gesture == input_gesture_dual_shift ? INPUT_SOURCE_PET : INPUT_SOURCE_BUTTON, gesture);
    return tui_action_none;
}

// Records which configuration the legacy menu selected.
static void record_menu_columns(void* context, options_t* options) {
    unsigned int* const columns = context;
    *columns = options->columns;
}

// Chords do not activate selections, while button short/long retain Down/Activate.
START_TEST(test_legacy_menu_control_dispatch) {
    test_register_file("/config.yaml",
        "configs:\n"
        "  - name: First\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 40\n"
        "  - name: Second\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 80\n");
    uint8_t screen_buffer[40 * 25];
    const window_t window = window_create(screen_buffer, 40, 25);
    unsigned int columns = 0;
    const setup_sink_t sink = {
        .context = &columns,
        .on_set_options = record_menu_columns,
        .system_state = &system_state,
    };
    legacy_menu_key_index = 0;
    mock_reset();
    mock_input_set_reader(legacy_menu_input);
    menu_config_show(&window, &sink, true);
    mock_input_set_reader(NULL);
    ck_assert_uint_eq(legacy_menu_key_index, count_of(menu_gestures));
    ck_assert_uint_eq(columns, 80);
    test_clear_files();
}
END_TEST

// The production menu buffers HID taps even with terminal routing left in CLI mode.
START_TEST(test_menu_hid_navigation_after_entry) {
    test_register_file("/config.yaml",
        "configs:\n"
        "  - name: First\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 40\n"
        "  - name: Second\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 80\n");
    uint8_t screen_buffer[40 * 25];
    const window_t window = window_create(screen_buffer, 40, 25);
    unsigned int columns = 0;
    const setup_sink_t sink = {
        .context = &columns,
        .on_set_options = record_menu_columns,
        .system_state = &system_state,
    };

    system_state.terminal_session = terminal_session_cli;
    input_routing_set_menu_active(false);
    menu_hid_input_index = 0;
    mock_reset();
    mock_input_set_reader(menu_hid_input);
    menu_config_show(&window, &sink, true);
    mock_input_set_reader(NULL);

    ck_assert_uint_eq(menu_hid_input_index, count_of(menu_hid_keys));
    ck_assert_uint_eq(columns, 80);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    test_clear_files();
}
END_TEST

// Creates report-level regression coverage for firmware menu and PET runtime input.
Suite* keyboard_suite(void) {
    Suite* const suite = suite_create("keyboard");
    TCase* const core = tcase_create("reports");
    tcase_add_checked_fixture(core, setup, teardown);
    tcase_add_test(core, test_menu_batched_taps);
    tcase_add_loop_test(core, test_core_source_edges_without_reports, 0, 2);
    tcase_add_loop_test(core, test_core_cancel_before_dispatch, 0, 4);
    tcase_add_loop_test(core, test_queued_lock_changes_mapping, 0, 2);
    tcase_add_test(core, test_caps_lock_press_without_release);
    tcase_add_test(core, test_cancel_uses_published_mapping);
    tcase_add_test(core, test_gesture_binding_table_and_priority);
    tcase_add_test(core, test_queue_reads_do_not_poll_or_dispatch);
    tcase_add_test(core, test_core_batch_orders_edges_internally);
    tcase_add_test(core, test_terminal_leaf_cancellation);
    tcase_add_test(core, test_core_backpressure_retains_led_sync);
    tcase_add_test(core, test_typed_requests_are_independent_of_navigation);
    tcase_add_test(core, test_router_suppresses_held_keys_and_modifiers);
    tcase_add_test(core, test_menu_held_key_reports);
    tcase_add_test(core, test_menu_navigation_keys);
    tcase_add_loop_test(core, test_runtime_navigation_uses_pet_matrix, 0, 2);
    tcase_add_loop_test(core, test_menu_queue_lifecycle, 0, 2);
    tcase_add_test(core, test_usb_menu_chord);
    tcase_add_test(core, test_usb_menu_chord_release_before_poll);
    tcase_add_test(core, test_usb_menu_chord_reset_before_poll);
    tcase_add_test(core, test_usb_menu_chord_removed_before_poll);
    tcase_add_test(core, test_usb_menu_chords_are_per_keyboard);
    tcase_add_test(core, test_usb_menu_chords_are_per_interface);
    tcase_add_test(core, test_usb_keyboard_disconnect_releases_keys_and_modifiers);
    tcase_add_test(core, test_menu_queue_removal_preserves_other_sources);
    tcase_add_test(core, test_menu_press_edges_are_per_interface);
    tcase_add_loop_test(core, test_menu_held_key_does_not_own_runtime_key, 0, 2);
    tcase_add_loop_test(core, test_pet_menu_chord, 0, 2);
    tcase_add_loop_test(core, test_pet_menu_chord_with_key_already_down, 0, 2);
    tcase_add_loop_test(core, test_pet_menu_chord_release_before_poll, 0, 2);
    tcase_add_loop_test(core, test_pet_menu_chord_typing_after_deadline, 0, 2);
    tcase_add_test(core, test_implicit_shift_does_not_open_menu);
    tcase_add_test(core, test_batched_shifted_reports_preserve_key_releases);
    tcase_add_loop_test(core, test_unshift_mapping_precedes_simultaneous_modifier, 0, 2);
    tcase_add_test(core, test_keymap_shift_overrides_survive_unchanged_reports);
    tcase_add_test(core, test_typing_blocks_menu_hold);
    tcase_add_loop_test(core, test_usb_menu_chord_with_other_key, 0, 3);
    tcase_add_test(core, test_usb_menu_chord_typing_after_deadline);
    tcase_add_test(core, test_usb_menu_block_is_per_interface);
    tcase_add_test(core, test_batched_menu_release_restarts_hold);
    tcase_add_test(core, test_legacy_menu_control_dispatch);
    tcase_add_test(core, test_menu_hid_navigation_after_entry);
    tcase_add_loop_test(core, test_shared_menu_order, 0, 2);
    tcase_add_test(core, test_shared_menu_capacity);
    tcase_add_test(core, test_menu_typing_and_transition);
    tcase_add_loop_test(core, test_terminal_usb_ownership, 0, 2);
    tcase_add_test(core, test_terminal_mapped_cell_ownership);
    tcase_add_test(core, test_release_uses_saved_mapping);
    tcase_add_test(core, test_cancel_runtime_injection);
    tcase_add_test(core, test_terminal_modifier_ownership);
    tcase_add_test(core, test_runtime_crlf_and_reset);
    tcase_add_test(core, test_runtime_injection_capacity);
    tcase_add_test(core, test_disconnect_preserves_terminal_key);
    tcase_add_test(core, test_terminal_menu_aliases);
    tcase_add_loop_test(core, test_physical_layout_isolation, 0, 2);
    suite_add_tcase(suite, core);
    return suite;
}
