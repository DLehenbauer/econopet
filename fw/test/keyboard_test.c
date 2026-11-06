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

// Starts each report trace with an empty keyboard and firmware menu routing.
static void setup(void) {
    mock_reset();
    mock_clock_set(0);
    expect_hid_output_report = false;
    keyboard_menu_set_active(false);
    saved_state = system_state;
    system_state.term_input_dest = term_input_to_firmware;
    system_state.term_mode = term_mode_video;
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    report_key(0, 0);
    dispatch_all_key_events();
    usb_keyboard_reset(&system_state);
    (void) keyboard_menu_requested();
    keyboard_menu_set_active(true);
}

// Releases any final press before restoring the shared machine configuration.
static void teardown(void) {
    report_key(0, 0);
    dispatch_key_events();
    usb_keyboard_reset(&system_state);
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    (void) keyboard_menu_requested();
    keyboard_menu_set_active(false);
    system_state = saved_state;
}

static unsigned int menu_hid_input_index;
static const uint8_t menu_hid_keys[] = {HID_KEY_ARROW_DOWN, HID_KEY_ENTER};

// Supplies buffered HID navigation to the production configuration menu.
static int menu_hid_input(void) {
    ck_assert_uint_lt(menu_hid_input_index, count_of(menu_hid_keys));
    report_key(menu_hid_keys[menu_hid_input_index++], 0);
    report_key(0, 0);
    const int key = keyboard_getch();
    ck_assert_msg(key != EOF, "HID navigation was not buffered for the menu");
    return key;
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
    keyboard_menu_set_active(false);
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
        keyboard_menu_set_active(false);
        ck_assert_int_eq(keyboard_getch(), EOF);
        keyboard_menu_set_active(true);
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

// A qualified USB chord released before polling remains a pending menu event.
START_TEST(test_usb_menu_chord_release_before_poll) {
    system_state.term_input_dest = term_input_to_pet;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    report_key(0, 0);
    ck_assert_int_eq(input_getch(), KEY_SHIFT_SHIFT);
    ck_assert_int_eq(input_getch(), EOF);
}
END_TEST

// Reset discards a qualified USB chord that has not yet been consumed.
START_TEST(test_usb_menu_chord_reset_before_poll) {
    system_state.term_input_dest = term_input_to_pet;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    report_key(0, 0);
    usb_keyboard_reset(&system_state);
    ck_assert_int_eq(input_getch(), EOF);
}
END_TEST

// Removing the source keyboard discards its unconsumed qualified chord.
START_TEST(test_usb_menu_chord_removed_before_poll) {
    expect_hid_output_report = true;
    usb_keyboard_added(1, 0);
    ck_assert(!expect_hid_output_report);

    system_state.term_input_dest = term_input_to_pet;
    const uint8_t both = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT;
    report_key(0, both);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    report_key(0, 0);
    usb_keyboard_removed(1, 0);
    ck_assert_int_eq(input_getch(), EOF);
}
END_TEST

// Reports and removal from one keyboard do not cancel another keyboard's chord.
START_TEST(test_usb_menu_chords_are_per_keyboard) {
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
    ck_assert(keyboard_menu_requested());

    usb_keyboard_removed(2, 0);
    ck_assert(keyboard_menu_requested());
    keyboard_menu_consume();
    usb_keyboard_removed(1, 0);
}
END_TEST

// Reports and removal from one HID interface do not cancel another interface's chord.
START_TEST(test_usb_menu_chords_are_per_interface) {
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
    ck_assert(keyboard_menu_requested());

    usb_keyboard_removed(1, 1);
    ck_assert(keyboard_menu_requested());
    keyboard_menu_consume();
    usb_keyboard_removed(1, 0);
}
END_TEST

// Modifier-only release and disconnect both release injected PET matrix keys.
START_TEST(test_usb_keyboard_disconnect_releases_keys_and_modifiers) {
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
    ck_assert(!keyboard_menu_requested());
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
    ck_assert_int_eq(keyboard_getch(), KEY_DOWN);
    ck_assert_int_eq(keyboard_getch(), EOF);
    usb_keyboard_removed(1, 0);
}
END_TEST

// Identical held navigation keys still produce one menu press edge per interface.
START_TEST(test_menu_press_edges_are_per_interface) {
    report_key_from(1, 0, HID_KEY_ARROW_DOWN, 0);
    report_key_from(2, 0, HID_KEY_ARROW_DOWN, 0);
    report_key_from(1, 0, 0, 0);
    report_key_from(2, 0, 0, 0);

    ck_assert_int_eq(keyboard_getch(), KEY_DOWN);
    ck_assert_int_eq(keyboard_getch(), KEY_DOWN);
    ck_assert_int_eq(keyboard_getch(), EOF);
    keyboard_source_removed(2, 0);
}
END_TEST

// Menu-only holds neither suppress runtime presses nor retain their releases.
START_TEST(test_menu_held_key_does_not_own_runtime_key) {
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    const usb_keymap_entry_t down = {.row = 0, .col = 0};
    keymap[HID_KEY_ARROW_DOWN] = down;
    usb_keyboard_reset(&system_state);

    report_key_from(1, 0, HID_KEY_ARROW_DOWN, 0);
    ck_assert_int_eq(keyboard_getch(), KEY_DOWN);
    keyboard_menu_set_active(false);
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
    keyboard_menu_consume();

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

// Both PET layouts retain a qualified chord when release precedes the next poll.
START_TEST(test_pet_menu_chord_release_before_poll) {
    system_state.term_input_dest = term_input_to_pet;
    system_state.pet_keyboard_model = _i == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    const key_event_t left = _i == 0 ? PET_KEY_LSHIFT_N : PET_KEY_LSHIFT_B;
    const key_event_t right = _i == 0 ? PET_KEY_RSHIFT_N : PET_KEY_RSHIFT_B;
    pet_key_matrix[PET_KEY_COL(left)] &= ~(1u << PET_KEY_ROW(left));
    pet_key_matrix[PET_KEY_COL(right)] &= ~(1u << PET_KEY_ROW(right));
    ck_assert(!keyboard_menu_requested());

    mock_clock_advance(INPUT_LONG_PRESS_US);
    pet_key_matrix[PET_KEY_COL(left)] |= 1u << PET_KEY_ROW(left);
    pet_key_matrix[PET_KEY_COL(right)] |= 1u << PET_KEY_ROW(right);
    ck_assert_int_eq(input_getch(), KEY_SHIFT_SHIFT);
    ck_assert_int_eq(input_getch(), EOF);
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
    dispatch_all_key_events();
    memcpy(pet_key_matrix, usb_key_matrix, sizeof(pet_key_matrix));
    ck_assert(!keyboard_menu_requested());
    report_key(0, 0);
    dispatch_all_key_events();
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

// Batched shifted reports cannot overflow the queue and lose key releases.
START_TEST(test_batched_shifted_reports_preserve_key_releases) {
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
    usb_keyboard_reset(&system_state);

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
    usb_keymap_entry_t* const keymap = system_state.usb_keymap_data[
        system_state.pet_keyboard_model][usb_keymap_kind_symbolic];
    const usb_keymap_entry_t a = {.row = 0, .col = 0, .unshift = true};
    const usb_keymap_entry_t b = {.row = 1, .col = 0};
    const usb_keymap_entry_t shift = {.row = 2, .col = 0};
    keymap[HID_KEY_A | 0x100] = a;
    keymap[HID_KEY_B] = b;
    keymap[HID_KEY_SHIFT_LEFT] = shift;
    usb_keyboard_reset(&system_state);

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
    usb_keyboard_reset(&system_state);

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

    system_state.term_input_dest = term_input_to_firmware;
    system_state.term_mode = term_mode_cli;
    keyboard_menu_set_active(false);
    menu_hid_input_index = 0;
    mock_input_set_reader(menu_hid_input);
    menu_config_show(&window, &sink, false);
    mock_input_set_reader(NULL);

    ck_assert_uint_eq(menu_hid_input_index, count_of(menu_hid_keys));
    ck_assert_uint_eq(columns, 80);
    ck_assert_int_eq(keyboard_getch(), EOF);
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
    tcase_add_loop_test(core, test_pet_menu_chord_release_before_poll, 0, 2);
    tcase_add_test(core, test_implicit_shift_does_not_open_menu);
    tcase_add_test(core, test_batched_shifted_reports_preserve_key_releases);
    tcase_add_loop_test(core, test_unshift_mapping_precedes_simultaneous_modifier, 0, 2);
    tcase_add_test(core, test_keymap_shift_overrides_survive_unchanged_reports);
    tcase_add_test(core, test_typing_restarts_menu_hold);
    tcase_add_test(core, test_batched_menu_release_restarts_hold);
    tcase_add_test(core, test_legacy_menu_control_dispatch);
    tcase_add_test(core, test_menu_hid_navigation_after_entry);
    suite_add_tcase(suite, core);
    return suite;
}
