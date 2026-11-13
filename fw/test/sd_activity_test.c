// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "sd_activity_test.h"

#include <setjmp.h>

#include "hardware/gpio.h"

#include "input/input.h"
#include "input/input_controls.h"
#include "input/input_routing.h"
#include "input/input_uart.h"
#include "ieee/ieee_drive.h"
#include "menu/menu.h"
#include "mock.h"
#include "pet.h"
#include "sd/activity.h"
#include "usb/keyboard.h"
#include "ui/cli.h"

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
static jmp_buf diagnostic_exit;
static void (*drive_service_observer)(void);
static unsigned int drive_services;

void __real_ieee_drive_task(void);

// Retain real drive servicing while optionally observing storage-wait progress.
void __wrap_ieee_drive_task(void) {
    __real_ieee_drive_task();
    if (drive_service_observer != NULL)
        drive_service_observer();
}

// Verify the runtime diagnostic begins only after storage is quiet and the CPU halted.
static void observe_diagnostic(void) {
    ck_assert(sd_activity_quiet());
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    longjmp(diagnostic_exit, 1);
}

// Observe the real reset pulse only after the required physical SD inactivity.
static void observe_reset(cpu_state_t state) {
    ck_assert(sd_activity_quiet());
    if (state == CPU_RESET)
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

// Sample a physical GPIO level through production input and debounce orchestration.
static void button_state(const bool pressed) {
    mock_input_button_level(pressed);
    input_task();
    mock_clock_advance(BUTTON_DEBOUNCE_US);
    input_task();
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
    system_state.terminal_session = terminal_session_remote;
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));
    shifts(false);
    button_state(false);
    ck_assert(mock_input_empty());

    for (unsigned int source = 0; source < 2; ++source) {
        if (source == 0) shifts(true);
        else button_state(true);
        mock_clock_advance(INPUT_LONG_PRESS_US - 1);
        ck_assert(mock_input_empty());

        // A slow failed operation does not delay a qualified gesture.
        operation_duration = INPUT_LONG_PRESS_US * 2;
        backend_result = BD_ERROR_DEVICE_ERROR;
        ck_assert_int_eq(device->program(device, buffer, expected_addr, expected_size), backend_result);
        ck_assert(!sd_activity_quiet());
        ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
        ck_assert(mock_input_empty());
        if (source == 0) shifts(false);
        else button_state(false);
        ck_assert(mock_input_empty());
    }

    // Moving from keyboard to button cannot inherit the keyboard's elapsed hold.
    shifts(true);
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    shifts(false);
    button_state(true);
    mock_clock_advance(1);
    ck_assert(mock_input_empty());
    mock_clock_advance(INPUT_LONG_PRESS_US - 1);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    button_state(false);
    ck_assert(mock_input_empty());

    // Short releases remain short despite recent SD activity.
    operation_duration = 0;
    button_state(true);
    ck_assert_int_eq(device->read(device, buffer, expected_addr, expected_size), backend_result);
    button_state(false);
    ck_assert_int_eq(mock_take_request().action, input_system_request_reset);
    ck_assert(mock_input_empty());

    // Classify at the threshold even if release is sampled before the next poll.
    button_state(true);
    mock_clock_advance(INPUT_LONG_PRESS_US - BUTTON_DEBOUNCE_US - 1);
    button_state(false);
    ck_assert_int_eq(mock_take_request().action, input_system_request_reset);
    button_state(true);
    mock_clock_advance(INPUT_LONG_PRESS_US - BUTTON_DEBOUNCE_US);
    ck_assert_int_eq(device->read(device, buffer, expected_addr, expected_size), backend_result);
    button_state(false);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    ck_assert(mock_input_empty());

    // Transitions suppress held gestures, but preserve already-emitted machine intent.
    button_state(true);
    button_state(false);
    input_controls_consume();
    ck_assert_int_eq(mock_take_request().action, input_system_request_reset);
    ck_assert(mock_input_empty());
    button_state(true);
    input_controls_consume();
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_input_empty());
    button_state(false);
    ck_assert(mock_input_empty());

    // Simultaneous qualified sources produce one event and consume both holds.
    shifts(true);
    button_state(true);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert_int_eq(mock_take_request().action, input_system_request_menu);
    ck_assert(mock_input_empty());
    button_state(false);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_input_empty());
    shifts(false);
    system_state = saved;
}

// Restart inactivity from inside servicing, while the wait leaves the CPU running.
static void observe_storage_wait(void) {
    ck_assert_int_eq(get_cpu(), CPU_READY);
    ++drive_services;
    if (drive_services == 2) {
        operation_duration = 0;
        ck_assert_int_eq(expected_device->read(expected_device, expected_buffer,
            expected_addr, expected_size), backend_result);
    }
}

// Runtime actions stay pending while ordinary input/drive iterations continue uninterrupted.
static void check_storage_actions(blockdevice_t* device, uint8_t* buffer) {
    input_routing_set_menu_active(false);
    system_state.terminal_session = terminal_session_remote;
    const usb_keymap_entry_t key = {.row = 2, .col = 2};
    system_state.usb_keymap_data[system_state.pet_keyboard_model][usb_keymap_kind_symbolic][HID_KEY_A] = key;
    keyboard_core_reset(&system_state);
    for (unsigned int source = 0; source < 3; ++source) {
        const unsigned int prior_resets = resets;
        const unsigned int prior_entries = entries;
        set_cpu(CPU_READY);
        const hid_keyboard_report_t pressed = {.keycode = {HID_KEY_A}};
        const hid_keyboard_report_t released = {0};
        mock_input_hid_report(2, 0, &pressed);
        input_task();
        ck_assert_uint_eq(mock_input_published_matrix()[key.col] & (1u << key.row), 0);
        if (source == 2) {
            const hid_keyboard_report_t chord = {
                .modifier = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT,
            };
            mock_input_hid_report(1, 0, &chord);
            input_task();
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
        input_task();
        ck_assert_int_eq(input_routing_pending_request().action,
            source == 0 ? input_system_request_reset : input_system_request_menu);
        mock_cpu_set_state_observer(observe_reset);
        drive_services = 0;
        const uint64_t started = time_us_64();
        pet_task();
        ck_assert_uint_eq(time_us_64(), started);
        ck_assert_uint_eq(drive_services, 0);
        ck_assert_uint_eq(resets, prior_resets);
        ck_assert_uint_eq(entries, prior_entries);
        ck_assert_int_eq(get_cpu(), CPU_READY);

        // Release USB and gesture holds while storage is busy. UART remains responsive.
        mock_input_hid_report(2, 0, &released);
        if (source == 2) mock_input_hid_report(1, 0, &released);
        else button_state(false);
        input_task();
        ck_assert_uint_eq(mock_input_published_matrix()[key.col], UINT8_MAX);
        const uint8_t character = 'a';
        mock_input_uart_bytes(&character, 1);
        input_task();
        ck_assert_uint_eq(mock_input_published_matrix()[key.col] & (1u << key.row), 0);
        input_task();
        input_task();
        ck_assert_uint_eq(mock_input_published_matrix()[key.col], UINT8_MAX);
        const uint8_t ctrl_c = 0x03;
        mock_input_uart_bytes(&ctrl_c, 1);
        input_task();
        ck_assert_int_eq(system_state.terminal_session, terminal_session_cli);
        ck_assert_int_eq(input_routing_pending_request().action,
            source == 0 ? input_system_request_reset : input_system_request_menu);
        system_state.terminal_session = terminal_session_remote;

        // A later button gesture cannot replace intent retained from an earlier observation.
        button_state(true);
        if (source == 0) {
            mock_clock_advance(INPUT_LONG_PRESS_US);
            operation_duration = 0;
            ck_assert_int_eq(device->read(device, buffer, expected_addr, expected_size), backend_result);
            input_task();
        }
        button_state(false);
        ck_assert_int_eq(input_routing_pending_request().action,
            source == 0 ? input_system_request_reset : input_system_request_menu);

        // Further I/O resets the quiet deadline without blocking input or consuming the request.
        operation_duration = 0;
        mock_clock_advance(SD_ACTIVITY_QUIET_US - 100000u);
        ieee_drive_task();
        backend_result = BD_ERROR_OK;
        ck_assert_int_eq(device->read(device, buffer, expected_addr, expected_size), backend_result);
        for (unsigned int iteration = 0; iteration < 4; ++iteration) {
            const uint64_t before = time_us_64();
            const unsigned int publications = mock_input_publish_count();
            const unsigned int usb_services = mock_input_usb_service_count();
            const unsigned int button_samples = mock_input_button_sample_count();
            input_task();
            pet_task();
            ck_assert_uint_eq(time_us_64(), before);
            ck_assert_uint_eq(mock_input_publish_count(), publications + 1);
            ck_assert_uint_eq(mock_input_usb_service_count(), usb_services + 1);
            ck_assert_uint_eq(mock_input_button_sample_count(), button_samples + 1);
            ck_assert_uint_eq(resets, prior_resets);
            ck_assert_uint_eq(entries, prior_entries);
            ck_assert_int_eq(get_cpu(), CPU_READY);
            mock_clock_advance(100000u);
        }
        mock_clock_advance(SD_ACTIVITY_QUIET_US - 400000u - 1u);
        pet_task();
        ck_assert_uint_eq(resets, prior_resets);
        ck_assert_uint_eq(entries, prior_entries);
        mock_clock_advance(1);
        pet_task();
        ck_assert_uint_eq(resets, prior_resets + (source == 0));
        ck_assert_uint_eq(entries, prior_entries + (source != 0));
        ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
        pet_task();
        ck_assert_uint_eq(resets, prior_resets + (source == 0));
        ck_assert_uint_eq(entries, prior_entries + (source != 0));
        mock_cpu_set_state_observer(NULL);
    }

    // Already-quiet SD needs no additional delay for a new short press.
    mock_cpu_set_state_observer(observe_reset);
    const unsigned int prior_resets = resets;
    button_state(true);
    button_state(false);
    input_task();
    const uint64_t started = time_us_64();
    pet_task();
    ck_assert_uint_lt(time_us_64() - started, 1000u);
    ck_assert_uint_eq(resets, prior_resets + 1);
    mock_cpu_set_state_observer(NULL);
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
    check_storage_actions(&device, buffer);

    // CLI execution waits synchronously before halting and entering the diagnostic.
    cli_init();
    operation_duration = 0;
    device.read(&device, buffer, expected_addr, expected_size);
    set_cpu(CPU_READY);
    drive_services = 0;
    drive_service_observer = observe_storage_wait;
    mock_ram_test_set_handler(observe_diagnostic);
    if (setjmp(diagnostic_exit) == 0) {
        const char* command = "test\r";
        while (*command) cli_process_char(*command++);
        ck_abort_msg("synchronous diagnostic did not run");
    }
    drive_service_observer = NULL;
    ck_assert_uint_gt(drive_services, 2);
    mock_ram_test_set_handler(NULL);
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
