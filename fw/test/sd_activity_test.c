// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "sd_activity_test.h"

#include "hardware/gpio.h"

#include "input.h"
#include "input_controls.h"
#include "menu/menu.h"
#include "mock.h"
#include "pet.h"
#include "sd/activity.h"
#include "usb/keyboard.h"

#define BUTTON_DEBOUNCE_US 50000u

static bool led_on;
static unsigned int led_changes;
static blockdevice_t* expected_device;
static const void* expected_buffer;
static bd_size_t expected_addr;
static bd_size_t expected_size;
static int backend_result;
static unsigned int reads;
static unsigned int writes;
static uint64_t operation_duration;
static unsigned int resets;
static unsigned int entries;

// Observe runtime resets only after the required physical SD inactivity.
void pet_reset(void) {
    ck_assert(sd_activity_quiet());
    ++resets;
}

// Observe runtime menu entry without running the interactive menu.
void menu_enter(bool is_boot) {
    ck_assert(!is_boot);
    ck_assert(sd_activity_quiet());
    ++entries;
}

// Record activity LED transitions made by the SD hooks.
void gpio_put(uint gpio, bool on) {
    ck_assert_uint_eq(gpio, PICO_DEFAULT_LED_PIN);
    led_on = on;
    ++led_changes;
}

// Check the forwarded block operation while the activity LED is on.
static int check_operation(blockdevice_t* device, const void* buffer, bd_size_t addr, bd_size_t size) {
    ck_assert(led_on);
    ck_assert_ptr_eq(device, expected_device);
    ck_assert_ptr_eq(buffer, expected_buffer);
    ck_assert_uint_eq(addr, expected_addr);
    ck_assert_uint_eq(size, expected_size);
    mock_clock_advance(operation_duration);
    return backend_result;
}

// Simulate the underlying SD read.
static int read_block(blockdevice_t* device, const void* buffer, bd_size_t addr, bd_size_t size) {
    ++reads;
    return check_operation(device, buffer, addr, size);
}

// Simulate the underlying SD write.
static int program_block(blockdevice_t* device, const void* buffer, bd_size_t addr, bd_size_t size) {
    ++writes;
    return check_operation(device, buffer, addr, size);
}

// Advances through the production debounce interval without generating GPIO mocks.
static void button_state(const bool pressed) {
    input_button_update(pressed);
    mock_clock_advance(BUTTON_DEBOUNCE_US);
    input_button_update(pressed);
}

// Supplies modifier-only USB input to the production gesture classifier.
static void shifts(const bool held) {
    const hid_keyboard_report_t report = {
        .modifier = held ? KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT : 0
    };
    process_kbd_report(1, 0, &report);
}

// SD activity never changes gesture classification, even if release precedes polling.
static void check_controls(blockdevice_t* device, uint8_t* buffer) {
    const system_state_t saved = system_state;
    system_state.term_input_dest = term_input_to_pet;
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));
    shifts(false);
    button_state(false);
    ck_assert_int_eq(input_getch(), EOF);

    for (unsigned int source = 0; source < 2; ++source) {
        if (source == 0) shifts(true);
        else button_state(true);
        mock_clock_advance(INPUT_LONG_PRESS_US - 1);
        ck_assert_int_eq(input_getch(), EOF);

        // A slow failed operation does not delay a qualified gesture.
        operation_duration = INPUT_LONG_PRESS_US * 2;
        backend_result = BD_ERROR_DEVICE_ERROR;
        ck_assert_int_eq(device->program(device, buffer, expected_addr, expected_size), backend_result);
        ck_assert(!sd_activity_quiet());
        ck_assert_int_eq(input_getch(), source == 0 ? KEY_SHIFT_SHIFT : KEY_BTN_LONG);
        ck_assert_int_eq(input_getch(), EOF);
        if (source == 0) shifts(false);
        else button_state(false);
        ck_assert_int_eq(input_getch(), EOF);
    }

    // Moving from keyboard to button cannot inherit the keyboard's elapsed hold.
    shifts(true);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    shifts(false);
    button_state(true);
    mock_clock_advance(1);
    ck_assert_int_eq(input_getch(), EOF);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert_int_eq(input_getch(), KEY_BTN_LONG);
    button_state(false);
    ck_assert_int_eq(input_getch(), EOF);

    // Short releases remain short despite recent SD activity.
    operation_duration = 0;
    button_state(true);
    ck_assert_int_eq(device->read(device, buffer, expected_addr, expected_size), backend_result);
    button_state(false);
    ck_assert_int_eq(input_getch(), KEY_BTN_SHORT);
    ck_assert_int_eq(input_getch(), EOF);

    // Classify at the threshold even if release is sampled before the next poll.
    button_state(true);
    mock_clock_advance(INPUT_LONG_PRESS_US - BUTTON_DEBOUNCE_US - 1);
    button_state(false);
    ck_assert_int_eq(input_getch(), KEY_BTN_SHORT);
    button_state(true);
    mock_clock_advance(INPUT_LONG_PRESS_US - BUTTON_DEBOUNCE_US);
    ck_assert_int_eq(device->read(device, buffer, expected_addr, expected_size), backend_result);
    button_state(false);
    ck_assert_int_eq(input_getch(), KEY_BTN_LONG);
    ck_assert_int_eq(input_getch(), EOF);

    // Context changes discard pending releases and consume currently held controls.
    button_state(true);
    button_state(false);
    input_controls_consume();
    ck_assert_int_eq(input_getch(), EOF);
    button_state(true);
    input_controls_consume();
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert_int_eq(input_getch(), EOF);
    button_state(false);
    ck_assert_int_eq(input_getch(), EOF);

    // Simultaneous qualified sources produce one event and consume both holds.
    shifts(true);
    button_state(true);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert_int_eq(input_getch(), KEY_BTN_LONG);
    ck_assert_int_eq(input_getch(), EOF);
    button_state(false);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert_int_eq(input_getch(), EOF);
    shifts(false);
    system_state = saved;
}

// Runtime actions stay pending across release and new I/O without blocking drive servicing.
static void check_deferred_actions(blockdevice_t* device, uint8_t* buffer) {
    for (unsigned int source = 0; source < 3; ++source) {
        const unsigned int prior_resets = resets;
        const unsigned int prior_entries = entries;
        if (source == 2) {
            shifts(true);
        } else {
            button_state(true);
        }
        if (source == 0) {
            button_state(false);
        } else {
            mock_clock_advance(INPUT_LONG_PRESS_US);
        }

        operation_duration = SD_ACTIVITY_QUIET_US * 2;
        backend_result = BD_ERROR_DEVICE_ERROR;
        ck_assert_int_eq(device->program(device, buffer, expected_addr, expected_size), backend_result);
        menu_task();
        ck_assert_uint_eq(resets, prior_resets);
        ck_assert_uint_eq(entries, prior_entries);

        // Releasing a recognized hold must not cancel or turn menu entry into reset.
        if (source == 2) shifts(false);
        else button_state(false);
        menu_task();
        ck_assert_uint_eq(resets, prior_resets);
        ck_assert_uint_eq(entries, prior_entries);

        // Later activity restarts the wait, including successful reads after failed writes.
        mock_clock_advance(SD_ACTIVITY_QUIET_US - 1);
        backend_result = BD_ERROR_OK;
        ck_assert_int_eq(device->read(device, buffer, expected_addr, expected_size), backend_result);
        menu_task();
        ck_assert_uint_eq(resets, prior_resets);
        ck_assert_uint_eq(entries, prior_entries);
        mock_clock_advance(SD_ACTIVITY_QUIET_US - 1);
        menu_task();
        ck_assert_uint_eq(resets, prior_resets);
        ck_assert_uint_eq(entries, prior_entries);
        mock_clock_advance(1);
        menu_task();
        ck_assert_uint_eq(resets, prior_resets + (source == 0));
        ck_assert_uint_eq(entries, prior_entries + (source != 0));
        menu_task();
        ck_assert_uint_eq(resets, prior_resets + (source == 0));
        ck_assert_uint_eq(entries, prior_entries + (source != 0));
    }

    // Already-quiet SD needs no additional delay for a new short press.
    const unsigned int prior_resets = resets;
    button_state(true);
    button_state(false);
    menu_task();
    ck_assert_uint_eq(resets, prior_resets + 1);
}

// Both successful and failed operations must preserve the backend result and clear the LED.
START_TEST(test_block_activity) {
    mock_reset();
    mock_clock_set(0);
    ck_assert(sd_activity_quiet());
    input_hold_t hold = {0};
    input_hold_update(&hold, true);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert(!input_hold_ready(&hold));
    mock_clock_advance(1);
    ck_assert(input_hold_ready(&hold));
    mock_clock_set(0);
    blockdevice_t device = {.read = read_block, .program = program_block};
    uint8_t buffer[512];
    expected_device = &device;
    expected_buffer = buffer;
    expected_addr = 1024;
    expected_size = sizeof(buffer);
    sd_activity_install(&device);

    backend_result = BD_ERROR_OK;
    ck_assert_int_eq(device.read(&device, buffer, expected_addr, expected_size), BD_ERROR_OK);
    ck_assert(!sd_activity_quiet());
    mock_clock_advance(SD_ACTIVITY_QUIET_US - 1);
    ck_assert(!sd_activity_quiet());
    mock_clock_advance(1);
    ck_assert(sd_activity_quiet());
    ck_assert(!led_on);
    ck_assert_uint_eq(led_changes, 2);
    ck_assert_uint_eq(reads, 1);
    ck_assert_uint_eq(writes, 0);

    ck_assert_int_eq(device.program(&device, buffer, expected_addr, expected_size), BD_ERROR_OK);
    ck_assert(!sd_activity_quiet());
    ck_assert(!led_on);
    ck_assert_uint_eq(led_changes, 4);
    ck_assert_uint_eq(reads, 1);
    ck_assert_uint_eq(writes, 1);

    backend_result = BD_ERROR_DEVICE_ERROR;
    ck_assert_int_eq(device.read(&device, buffer, expected_addr, expected_size), BD_ERROR_DEVICE_ERROR);
    ck_assert(!led_on);
    ck_assert_uint_eq(led_changes, 6);
    ck_assert_uint_eq(reads, 2);
    ck_assert_uint_eq(writes, 1);

    ck_assert_int_eq(device.program(&device, buffer, expected_addr, expected_size), BD_ERROR_DEVICE_ERROR);
    ck_assert(!led_on);
    ck_assert_uint_eq(led_changes, 8);
    ck_assert_uint_eq(reads, 2);
    ck_assert_uint_eq(writes, 2);
    check_controls(&device, buffer);
    check_deferred_actions(&device, buffer);
}
END_TEST

// Register SD block activity checks.
Suite* sd_activity_suite(void) {
    Suite* const suite = suite_create("SD activity");
    TCase* const checks = tcase_create("block operations");
    tcase_add_test(checks, test_block_activity);
    suite_add_tcase(suite, checks);
    return suite;
}
