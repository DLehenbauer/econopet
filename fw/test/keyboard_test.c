// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "keyboard_test.h"

#include "input.h"
#include "input_controls.h"
#include "menu/menu_config.h"
#include "mock.h"
#include "usb/keyboard.h"
#include "usb/keyscan.h"

static system_state_t saved_state;

// Fails if these report-only tests unexpectedly try to control a physical keyboard.
bool tuh_hid_set_report(uint8_t dev_addr, uint8_t idx, uint8_t report_id, uint8_t report_type,
                        void* report, uint16_t len) {
    (void) dev_addr;
    (void) idx;
    (void) report_id;
    (void) report_type;
    (void) report;
    (void) len;
    ck_abort_msg("unexpected USB output report in keyboard input test");
    return false;
}

// Delivers a real HID report without dispatching the PET matrix or polling menu input.
static void report_key(uint8_t keycode, uint8_t modifiers) {
    const hid_keyboard_report_t report = {.modifier = modifiers, .keycode = {keycode}};
    process_kbd_report(1, &report);
}

// Starts each report trace with an empty keyboard and firmware menu routing.
static void setup(void) {
    mock_reset();
    mock_clock_set(0);
    saved_state = system_state;
    system_state.term_input_dest = term_input_to_firmware;
    system_state.term_mode = term_mode_video;
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    report_key(0, 0);
    dispatch_key_events();
    usb_keyboard_reset(&system_state);
    (void) keyboard_menu_requested();
}

// Releases any final press before restoring the shared machine configuration.
static void teardown(void) {
    report_key(0, 0);
    dispatch_key_events();
    usb_keyboard_reset(&system_state);
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    (void) keyboard_menu_requested();
    system_state = saved_state;
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
        dispatch_key_events();
        for (size_t i = 0; i < count_of(expected); i++)
            ck_assert_int_eq(keyboard_getch(), expected[i]);
        ck_assert_int_eq(keyboard_getch(), EOF);
        for (size_t i = 0; i < count_of(usb_key_matrix); i++)
            ck_assert_uint_eq(usb_key_matrix[i], 0xff);
    }
}
END_TEST

// Unchanged held-key reports are not repeated, but release/repress creates another press.
START_TEST(test_menu_held_key_reports) {
    report_key(HID_KEY_ARROW_DOWN, 0);
    report_key(HID_KEY_ARROW_DOWN, 0);
    ck_assert_int_eq(keyboard_getch(), KEY_DOWN);
    ck_assert_int_eq(keyboard_getch(), EOF);
    report_key(0, 0);
    report_key(HID_KEY_ARROW_DOWN, 0);
    ck_assert_int_eq(keyboard_getch(), KEY_DOWN);
    ck_assert_int_eq(keyboard_getch(), EOF);
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
        ck_assert_int_eq(keyboard_getch(), expected[i]);
    }
    ck_assert_int_eq(keyboard_getch(), EOF);
}
END_TEST

// Navigation outside the menu still reaches the configured PET keyboard matrix.
START_TEST(test_runtime_navigation_uses_pet_matrix) {
    system_state.term_input_dest = term_input_to_pet;
    const key_event_t down = PET_KEY_DOWN_N;
    usb_keymap_entry_t* const entry = &system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_ARROW_DOWN];
    *entry = (usb_keymap_entry_t) {.row = PET_KEY_ROW(down), .col = PET_KEY_COL(down)};
    usb_keyboard_reset(&system_state);
    report_key(HID_KEY_ARROW_DOWN, 0);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[PET_KEY_COL(down)], 0xff & ~(1u << PET_KEY_ROW(down)));
    if (_i != 0) system_state.term_input_dest = term_input_to_firmware;
    report_key(0, 0);
    dispatch_key_events();
    ck_assert_uint_eq(usb_key_matrix[PET_KEY_COL(down)], 0xff);
    ck_assert_int_eq(keyboard_getch(), EOF);
}
END_TEST

// Reset and leaving menu routing discard unread presses instead of replaying them later.
START_TEST(test_menu_queue_lifecycle) {
    report_key(HID_KEY_ENTER, 0);
    report_key(0, 0);
    if (_i == 0) usb_keyboard_reset(&system_state);
    else {
        system_state.term_input_dest = term_input_to_pet;
        ck_assert_int_eq(keyboard_getch(), EOF);
        system_state.term_input_dest = term_input_to_firmware;
    }
    ck_assert_int_eq(keyboard_getch(), EOF);
}
END_TEST

// Only a continuous raw dual Shift hold qualifies, with release required after consumption.
START_TEST(test_usb_menu_chord) {
    system_state.term_input_dest = term_input_to_pet;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, KEYBOARD_MODIFIER_LEFTSHIFT);
    ck_assert(!keyboard_menu_requested());
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(1);
    ck_assert(keyboard_menu_requested());
    keyboard_menu_consume();
    ck_assert(!keyboard_menu_requested());
    report_key(0, both);
    ck_assert(!keyboard_menu_requested());
    usb_keyboard_reset(&system_state);
    report_key(0, both);
    ck_assert(!keyboard_menu_requested());
    report_key(0, KEYBOARD_MODIFIER_RIGHTSHIFT);
    report_key(0, both);
    report_key(0, 0);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!keyboard_menu_requested());

    // Routing does not change recognition. Dispatch can ignore the consumed event.
    system_state.term_input_dest = term_input_to_firmware;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert_int_eq(input_getch(), KEY_SHIFT_SHIFT);
    system_state.term_input_dest = term_input_to_pet;
    ck_assert(!keyboard_menu_requested());
    report_key(0, both);
    ck_assert(!keyboard_menu_requested());
}
END_TEST

// PET business and graphics keyboards require their own two physical Shift keys.
START_TEST(test_pet_menu_chord) {
    system_state.term_input_dest = term_input_to_pet;
    system_state.pet_keyboard_model = _i == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    const key_event_t left = _i == 0 ? PET_KEY_LSHIFT_N : PET_KEY_LSHIFT_B;
    const key_event_t right = _i == 0 ? PET_KEY_RSHIFT_N : PET_KEY_RSHIFT_B;
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    ck_assert(!keyboard_menu_requested());
    pet_key_matrix[PET_KEY_COL(right)] &= ~(1u << PET_KEY_ROW(right));
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(keyboard_menu_requested());
    keyboard_menu_consume();
    ck_assert(!keyboard_menu_requested());
    pet_key_matrix[PET_KEY_COL(left)] |= 1u << PET_KEY_ROW(left);
    ck_assert(!keyboard_menu_requested());
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(keyboard_menu_requested());

    // Other physical keys cancel qualification even while Shift-lock stays down.
    const key_event_t typing = _i == 0 ? PET_KEY_T_N : PET_KEY_T_B;
    pet_key_matrix[PET_KEY_COL(typing)] &= ~(1u << PET_KEY_ROW(typing));
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!keyboard_menu_requested());
    pet_key_matrix[PET_KEY_COL(typing)] |= 1u << PET_KEY_ROW(typing);
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(1);
    ck_assert(keyboard_menu_requested());

    // Menu dispatch can ignore the physical chord without replaying it on exit.
    system_state.term_input_dest = term_input_to_firmware;
    ck_assert_int_eq(input_getch(), KEY_SHIFT_SHIFT);
    ck_assert(!keyboard_menu_requested());
    system_state.term_input_dest = term_input_to_pet;
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!keyboard_menu_requested());
}
END_TEST

// A keymap can inject left Shift for right-Shift-A. That is not a two-Shift chord.
START_TEST(test_implicit_shift_does_not_open_menu) {
    system_state.term_input_dest = term_input_to_pet;
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
    usb_keyboard_reset(&system_state);
    report_key(HID_KEY_A, KEYBOARD_MODIFIER_RIGHTSHIFT);
    dispatch_key_events();
    memcpy(pet_key_matrix, usb_key_matrix, sizeof(pet_key_matrix));
    ck_assert(!keyboard_menu_requested());
    report_key(0, 0);
    dispatch_key_events();
    ck_assert(!keyboard_menu_requested());
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    ck_assert(!keyboard_menu_requested());
    pet_key_matrix[PET_KEY_COL(PET_KEY_LSHIFT_N)] &= ~(1u << PET_KEY_ROW(PET_KEY_LSHIFT_N));
    pet_key_matrix[PET_KEY_COL(PET_KEY_RSHIFT_N)] &= ~(1u << PET_KEY_ROW(PET_KEY_RSHIFT_N));
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(keyboard_menu_requested());
}
END_TEST

// Typing with Shift-lock restarts the hold instead of qualifying during a key press.
START_TEST(test_typing_restarts_menu_hold) {
    system_state.term_input_dest = term_input_to_pet;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    report_key(HID_KEY_A, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(!keyboard_menu_requested());
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(1);
    ck_assert_int_eq(input_getch(), KEY_SHIFT_SHIFT);
    ck_assert_int_eq(input_getch(), EOF);
}
END_TEST

// USB release/repress within a servicing batch cannot inherit the previous hold.
START_TEST(test_batched_menu_release_restarts_hold) {
    system_state.term_input_dest = term_input_to_pet;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    report_key(0, 0);
    report_key(0, both);
    mock_clock_advance(1);
    ck_assert(!keyboard_menu_requested());
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert(keyboard_menu_requested());
}
END_TEST

static size_t legacy_menu_key_index;
static const int legacy_menu_keys[] = {
    KEY_SHIFT_SHIFT, KEY_BTN_SHORT, KEY_SHIFT_SHIFT, KEY_BTN_LONG,
};

// Supplies a bounded trace so an ignored activation cannot hang the menu test.
static int legacy_menu_input(void) {
    ck_assert_uint_lt(legacy_menu_key_index, count_of(legacy_menu_keys));
    return legacy_menu_keys[legacy_menu_key_index++];
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
    mock_input_set_reader(legacy_menu_input);
    menu_config_show(&window, &sink, false);
    mock_input_set_reader(NULL);
    ck_assert_uint_eq(legacy_menu_key_index, count_of(legacy_menu_keys));
    ck_assert_uint_eq(columns, 80);
    test_clear_files();
}
END_TEST

// Creates report-level regression coverage for firmware menu and PET runtime input.
Suite* keyboard_suite(void) {
    Suite* const suite = suite_create("keyboard");
    TCase* const core = tcase_create("reports");
    tcase_add_checked_fixture(core, setup, teardown);
    tcase_add_test(core, test_menu_batched_taps);
    tcase_add_test(core, test_menu_held_key_reports);
    tcase_add_test(core, test_menu_navigation_keys);
    tcase_add_loop_test(core, test_runtime_navigation_uses_pet_matrix, 0, 2);
    tcase_add_loop_test(core, test_menu_queue_lifecycle, 0, 2);
    tcase_add_test(core, test_usb_menu_chord);
    tcase_add_loop_test(core, test_pet_menu_chord, 0, 2);
    tcase_add_test(core, test_implicit_shift_does_not_open_menu);
    tcase_add_test(core, test_typing_restarts_menu_hold);
    tcase_add_test(core, test_batched_menu_release_restarts_hold);
    tcase_add_test(core, test_legacy_menu_control_dispatch);
    suite_add_tcase(suite, core);
    return suite;
}
