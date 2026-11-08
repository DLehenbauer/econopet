// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input_test.h"

#include "diag/log/log.h"
#include "display/display.h"
#include "input/input.h"
#include "input/input_controls.h"
#include "input/input_routing.h"
#include "input/keyboard_core.h"
#include "mock.h"
#include "ui/cli.h"
#include "usb/keyboard.h"
#include "usb/keyscan.h"

#define TEST_BUTTON_DEBOUNCE_US 50000u
#define TEST_ESCAPE_TIMEOUT_US 10000u
#define TEST_USB_DEVICE 3u

static system_state_t saved_state;

// Queue an actual terminal byte stream without observing it until input_task runs.
static void uart_bytes(const char* text) {
    mock_input_uart_bytes((const uint8_t*)text, strlen(text));
}

// Queue a six-key transport report at the USB host boundary.
static void hid_key(uint8_t key, uint8_t modifiers) {
    const hid_keyboard_report_t report = {.modifier = modifiers, .keycode = {key}};
    mock_input_hid_report(TEST_USB_DEVICE, 0, &report);
}

// Begin with independent physical snapshots, released sources, and a deterministic clock.
static void setup(void) {
    saved_state = system_state;
    mock_reset();
    mock_clock_set(0);
    log_init();
    keyboard_core_reset(&system_state);
    hid_key(0, 0);
    input_task();
    mock_clock_advance(TEST_BUTTON_DEBOUNCE_US);
    input_task();
    input_controls_consume();
    system_state.terminal_session = terminal_session_remote;
}

// Release raw report history before retiring routed ownership and restoring configuration.
static void teardown(void) {
    hid_key(0, 0);
    mock_input_button_level(false);
    input_task();
    mock_clock_advance(TEST_BUTTON_DEBOUNCE_US);
    input_task();
    input_routing_reset();
    keyboard_source_removed(TEST_USB_DEVICE, 0);
    system_state = saved_state;
}

// Pure reads cannot acquire bytes, deliver reports, sample GPIO, or exchange FPGA state.
START_TEST(test_production_observation_order_and_passive_reads) {
    input_routing_set_menu_active(true);
    uart_bytes("\x1b[A");
    hid_key(HID_KEY_ARROW_RIGHT, 0);
    uint8_t physical[KEY_COL_COUNT];
    memset(physical, UINT8_MAX, sizeof(physical));
    const key_event_t enter = system_state.pet_keyboard_model == pet_keyboard_model_graphics
        ? PET_KEY_RETURN_N : PET_KEY_RETURN_B;
    physical[PET_KEY_COL(enter)] &= (uint8_t)~(1u << PET_KEY_ROW(enter));
    mock_input_pet_snapshot(physical);
    mock_input_button_level(true);
    const unsigned int published = mock_input_publish_count();
    const unsigned int usb = mock_input_usb_service_count();
    const unsigned int hid = mock_input_hid_service_count();
    const unsigned int button = mock_input_button_sample_count();
    const uint64_t before = time_us_64();
    for (size_t i = 0; i < 3; ++i) {
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
        ck_assert_int_eq(input_routing_take_request().action, input_system_none);
    }
    ck_assert_uint_eq(mock_input_publish_count(), published);
    ck_assert_uint_eq(mock_input_usb_service_count(), usb);
    ck_assert_uint_eq(mock_input_button_sample_count(), button);
    input_task();
    ck_assert_uint_eq(time_us_64(), before);
    ck_assert_uint_eq(mock_input_publish_count(), published + 1);
    ck_assert_uint_eq(mock_input_usb_service_count(), usb + 1);
    ck_assert_uint_eq(mock_input_hid_service_count(), hid + 1);
    ck_assert_uint_eq(mock_input_button_sample_count(), button + 1);
    ck_assert_int_eq(input_routing_navigation(), tui_action_up);
    ck_assert_int_eq(input_routing_navigation(), tui_action_page_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_activate);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    ck_assert_int_eq(input_routing_pending_request().action, input_system_none);

    // The physical button is timed only by observed levels, then bound to menu navigation.
    mock_clock_advance(TEST_BUTTON_DEBOUNCE_US);
    input_task();
    mock_input_button_level(false);
    input_task();
    mock_clock_advance(TEST_BUTTON_DEBOUNCE_US);
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
}
END_TEST

// Runtime key-down and key-up reach the SPI publication boundary in their actual input cycles.
START_TEST(test_production_runtime_publication) {
    input_routing_set_menu_active(false);
    const usb_keymap_entry_t uart_cell = {.row = 2, .col = 3};
    const usb_keymap_entry_t usb_cell = {.row = 3, .col = 3};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A] = uart_cell;
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_B] = usb_cell;
    keyboard_core_reset(&system_state);
    uint8_t crtc[CRTC_REG_COUNT];
    memset(crtc, 0x12, sizeof(crtc));
    mock_input_crtc_snapshot(crtc);
    mock_set_text_mode_lower(false);
    uart_bytes("a");
    hid_key(HID_KEY_B, 0);
    input_task();
    const uint8_t mask = (1u << uart_cell.row) | (1u << usb_cell.row);
    ck_assert_uint_eq(mock_input_published_matrix()[uart_cell.col] & mask, 0);
    ck_assert_int_eq(system_state.video_text_mode, video_text_mode_graphics);
    ck_assert_int_eq(memcmp(system_state.pet_crtc_registers, crtc, sizeof(crtc)), 0);
    hid_key(0, 0);
    input_task();
    ck_assert_uint_eq(mock_input_published_matrix()[uart_cell.col] & (1u << uart_cell.row), 0);
    ck_assert_uint_ne(mock_input_published_matrix()[usb_cell.col] & (1u << usb_cell.row), 0);
    input_task();
    ck_assert_uint_eq(mock_input_published_matrix()[uart_cell.col], UINT8_MAX);
}
END_TEST

// Every supported escape and every fragmentation point use the production byte adapter.
START_TEST(test_incremental_escape_aliases) {
    static const struct {
        const char* bytes;
        tui_action_t action;
    } cases[] = {
        {"\x1b[A", tui_action_up}, {"\x1b[B", tui_action_down},
        {"\x1b[C", tui_action_page_down}, {"\x1b[D", tui_action_page_up},
        {"\x1b[F", tui_action_none}, {"\x1b[H", tui_action_none},
        {"\x1b[5~", tui_action_page_up}, {"\x1b[6~", tui_action_page_down},
    };
    input_routing_set_menu_active(true);
    for (size_t i = 0; i < count_of(cases); ++i) {
        const size_t length = strlen(cases[i].bytes);
        for (size_t split = 1; split < length; ++split) {
            mock_input_uart_bytes((const uint8_t*)cases[i].bytes, split);
            const uint64_t before = time_us_64();
            input_task();
            ck_assert_uint_eq(time_us_64(), before);
            ck_assert_int_eq(input_routing_navigation(), tui_action_none);
            mock_clock_advance(TEST_ESCAPE_TIMEOUT_US - 1u);
            mock_input_uart_bytes((const uint8_t*)cases[i].bytes + split, length - split);
            input_task();
            ck_assert_int_eq(input_routing_navigation(), cases[i].action);
            ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        }
    }
}
END_TEST

// Buffered suffixes belong to their escape even when rendering delayed the next poll.
START_TEST(test_buffered_escape_after_delayed_poll) {
    static const struct {
        const char* prefix;
        const char* suffix;
        tui_action_t action;
    } cases[] = {
        {"\x1b", "[A", tui_action_up},
        {"\x1b[", "6~", tui_action_page_down},
    };
    input_routing_set_menu_active(true);
    for (size_t i = 0; i < count_of(cases); ++i) {
        uart_bytes(cases[i].prefix);
        input_task();
        mock_clock_advance(TEST_ESCAPE_TIMEOUT_US + 1u);
        uart_bytes(cases[i].suffix);
        input_task();
        ck_assert_int_eq(input_routing_navigation(), cases[i].action);
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    }
}
END_TEST

// An unfinished escape cannot postpone USB releases, button timing, or FPGA publication.
START_TEST(test_escape_timeout_keeps_other_sources_serviced) {
    input_routing_set_menu_active(false);
    const usb_keymap_entry_t cell = {.row = 2, .col = 3};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_B] = cell;
    keyboard_core_reset(&system_state);
    hid_key(HID_KEY_B, 0);
    input_task();
    ck_assert_uint_eq(mock_input_published_matrix()[cell.col] & (1u << cell.row), 0);
    uart_bytes("\x1b");
    hid_key(0, 0);
    mock_input_button_level(true);
    const uint64_t before = time_us_64();
    const unsigned int publications = mock_input_publish_count();
    input_task();
    ck_assert_uint_eq(time_us_64(), before);
    ck_assert_uint_eq(mock_input_publish_count(), publications + 1);
    ck_assert_uint_eq(mock_input_published_matrix()[cell.col], UINT8_MAX);
    input_routing_set_menu_active(true);
    mock_clock_advance(TEST_ESCAPE_TIMEOUT_US - 1u);
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    mock_clock_advance(1);
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_back);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    mock_clock_advance(TEST_BUTTON_DEBOUNCE_US);
    input_task();
    mock_clock_advance(INPUT_LONG_PRESS_US);
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_activate);
    ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
}
END_TEST

// Expired and unsupported CSI sequences do not become navigation or poison later bytes.
START_TEST(test_escape_recovery) {
    input_routing_set_menu_active(true);
    uart_bytes("\x1b[");
    input_task();
    mock_clock_advance(TEST_ESCAPE_TIMEOUT_US);
    uart_bytes("\x1b[A");
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_up);
    uart_bytes("\x1b[55~\x1b[1A\x1b[1234567890\x1b[B");
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    uart_bytes("\x1b\r\n");
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_activate);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
}
END_TEST

// CRLF normalization and CLI/Remote changes occur inside actual scheduled acquisition.
START_TEST(test_acquired_terminal_session_transitions) {
    cli_init();
    uart_bytes("remote\r\n");
    input_task();
    ck_assert(cli_in_remote_mode());
    ck_assert(display_term_video_active());
    ck_assert_uint_eq(mock_term_inject_count(), 0);
    input_routing_set_menu_active(true);
    uart_bytes("\x1b[B\r");
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    ck_assert_int_eq(input_routing_navigation(), tui_action_activate);
    input_routing_set_menu_active(false);
    uart_bytes("\n");
    input_task();
    ck_assert_uint_eq(mock_term_inject_count(), 0);
    input_routing_request((input_system_request_t){INPUT_SOURCE_BUTTON, input_system_request_reset});
    uart_bytes("\x03");
    input_task();
    ck_assert(!cli_in_remote_mode());
    ck_assert(!display_term_video_active());
    ck_assert_int_eq(input_routing_take_request().action, input_system_request_reset);
}
END_TEST

Suite* input_suite(void) {
    Suite* const suite = suite_create("input");
    TCase* const task = tcase_create("hardware-boundaries");
    tcase_add_checked_fixture(task, setup, teardown);
    tcase_add_test(task, test_production_observation_order_and_passive_reads);
    tcase_add_test(task, test_production_runtime_publication);
    tcase_add_test(task, test_incremental_escape_aliases);
    tcase_add_test(task, test_buffered_escape_after_delayed_poll);
    tcase_add_test(task, test_escape_timeout_keeps_other_sources_serviced);
    tcase_add_test(task, test_escape_recovery);
    tcase_add_test(task, test_acquired_terminal_session_transitions);
    suite_add_tcase(suite, task);
    return suite;
}
