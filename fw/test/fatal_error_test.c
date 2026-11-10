// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "fatal_error_test.h"

#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hardware/gpio.h"

#include "diag/log/log.h"
#include "fatal.h"
#include "input/input.h"
#include "input/input_routing.h"
#include "mock.h"
#include "roms/roms.h"
#include "sd/activity.h"
#include "usb/keyscan.h"

static jmp_buf fatal_exit;
static bool observing;
static bool led_on;
static bool led_initialized;
static bool led_output;
static bool halt_only_called;
static bool observing_rom_boot;
static pet_keyboard_model_t rom_keyboard_model = pet_keyboard_model_graphics;
static unsigned int keymap_reads;
static unsigned int model_reads;
static unsigned int model_writes;
static size_t stdio_init_count;
static size_t delay_count;
static size_t delay_limit;
static size_t pattern_delay_count;
static off_t first_message_size;
#define FATAL_LOG_DUMP_TEST_ELAPSED_US (11u * 1000u * 1000u)
#define MORSE_CAPTURE_CAPACITY 19u
static struct {
    bool on;
    uint64_t duration;
} delays[MORSE_CAPTURE_CAPACITY];

void __real_gpio_put(unsigned int gpio, bool value);
void __real_sleep_us(uint64_t us);
void __real_fatal_halt(fatal_error_t error, const char* message) __attribute__((noreturn));

// Keep ordinary startup-failure tests aborting while Morse tests call the real endpoint.
void __wrap_fatal_halt(fatal_error_t error, const char* message) {
    (void)error;
    halt_only_called = true;
    fatal("%s", message);
}

// Supply known DIP-switch observations while the ROM loader holds the CPU halted.
void read_pet_model(system_state_t* state) {
    ck_assert_msg(observing_rom_boot, "unexpected PET model read outside ROM boot tests");
    ck_assert_ptr_eq(state, &system_state);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    state->pet_video_type = pet_video_type_fixed;
    state->pet_keyboard_model = rom_keyboard_model;
    ++model_reads;
}

// Check the native display setup requested before starting either menu ROM entry.
void write_pet_model(const system_state_t* state) {
    ck_assert_msg(observing_rom_boot, "unexpected PET model write outside ROM boot tests");
    ck_assert_ptr_eq(state, &system_state);
    ck_assert_int_eq(state->pet_display_columns, pet_display_columns_40);
    ck_assert_uint_eq(state->video_ram_mask, pet_video_ram_mask_1kb);
    ++model_writes;
}

// Observe SD keymap requests without depending on a physical SD card.
void read_keymap(const char* filename, system_state_t* state) {
    ck_assert_msg(observing_rom_boot, "unexpected keymap read outside ROM boot tests");
    ck_assert_ptr_eq(state, &system_state);
    ck_assert_str_eq(filename, "/ukm/us.bin");
    ++keymap_reads;
}

// Model successful serial initialization once, before any fatal output is emitted.
bool stdio_init_all(void) {
    ck_assert_msg(observing, "unexpected stdio initialization outside fatal diagnostics");
    ck_assert_uint_eq(stdio_init_count, 0);
    struct stat status;
    ck_assert_int_eq(fstat(fileno(stdout), &status), 0);
    ck_assert_int_eq(status.st_size, 0);
    ++stdio_init_count;
    return true;
}

// Observe fatal LED initialization independently of main() and SD activity.
void gpio_init(unsigned int gpio) {
    ck_assert_msg(observing, "unexpected GPIO initialization outside fatal diagnostics");
    ck_assert_uint_eq(stdio_init_count, 1);
    ck_assert_uint_eq(gpio, PICO_DEFAULT_LED_PIN);
    led_initialized = true;
    led_output = false;
}

// Require the fatal handler to configure the initialized LED as an output.
void gpio_set_dir(unsigned int gpio, bool output) {
    ck_assert_msg(observing, "unexpected GPIO direction change outside fatal diagnostics");
    ck_assert_uint_eq(gpio, PICO_DEFAULT_LED_PIN);
    ck_assert(led_initialized);
    ck_assert(output);
    led_output = output;
}

// Observe the error LED without changing the existing SD activity mock.
void __wrap_gpio_put(unsigned int gpio, bool value) {
    if (!observing) {
        __real_gpio_put(gpio, value);
        return;
    }
    ck_assert_uint_eq(gpio, PICO_DEFAULT_LED_PIN);
    ck_assert(led_initialized);
    ck_assert(led_output);
    led_on = value;
}

// Capture two full letters and escape the fatal loop without waiting in real time.
void __wrap_sleep_us(uint64_t us) {
    if (!observing) {
        __real_sleep_us(us);
        return;
    }
    mock_clock_advance(us);

    // Let the next complete Morse cycle reach the periodic diagnostic replay.
    if (delay_limit > 1 && delay_count == delay_limit - 2) {
        mock_clock_advance(FATAL_LOG_DUMP_TEST_ELAPSED_US);
    }

    // Check emitted bytes before the test's final flush can hide a missing flush.
    if (delay_count == 0 || delay_count == pattern_delay_count ||
        delay_count == 2 * pattern_delay_count) {
        struct stat status;
        ck_assert_int_eq(fstat(fileno(stdout), &status), 0);
        if (delay_count == 0) {
            ck_assert_int_gt(status.st_size, 0);
            first_message_size = status.st_size;
        } else if (delay_count == pattern_delay_count) {
            ck_assert_int_eq(status.st_size, first_message_size);
        } else {
            ck_assert_int_eq(status.st_size, 2 * first_message_size);
        }
    }

    ck_assert_uint_lt(delay_count, count_of(delays));
    delays[delay_count].on = led_on;
    delays[delay_count++].duration = us;
    if (delay_count == delay_limit) {
        longjmp(fatal_exit, 1);
    }
}

// Capture the initial and periodic reports across repeated Morse patterns.
static void check_fatal_reporting(fatal_error_t error, const char* diagnosis, const uint64_t* expected,
                                  size_t expected_count, bool with_history) {
    // Isolate retained history and include every level in a deliberately mixed sequence.
    mock_clock_set(0);
    log_init();
    if (with_history) {
        log_info("startup context");
        log_debug("ROM read context");
        log_warn("timeout context");
    }
    FILE* const output = tmpfile();
    ck_assert_ptr_nonnull(output);
    fflush(stdout);
    const int saved_stdout = dup(fileno(stdout));
    ck_assert_int_ge(saved_stdout, 0);
    ck_assert_int_ge(dup2(fileno(output), fileno(stdout)), 0);

    delay_count = 0;
    stdio_init_count = 0;
    pattern_delay_count = expected_count;
    delay_limit = 2 * expected_count + 1;
    ck_assert_uint_le(delay_limit, count_of(delays));
    led_on = true;
    led_initialized = false;
    led_output = false;
    observing = true;
    if (setjmp(fatal_exit) == 0) {
        __real_fatal_halt(error, "test fatal details");
    }
    observing = false;
    ck_assert_uint_eq(stdio_init_count, 1);
    fflush(stdout);
    ck_assert_int_ge(dup2(saved_stdout, fileno(stdout)), 0);
    close(saved_stdout);

    rewind(output);
    char message[1024] = {0};
    ck_assert_uint_lt(first_message_size, sizeof(message));
    ck_assert_uint_eq(fread(message, 1, first_message_size, output), first_message_size);
    ck_assert_ptr_nonnull(strstr(message, diagnosis));
    ck_assert_ptr_nonnull(strstr(message, "test fatal details"));
    char repeated_message[sizeof(message)] = {0};
    ck_assert_uint_eq(fread(repeated_message, 1, first_message_size, output), first_message_size);
    ck_assert_str_eq(repeated_message, message);
    ck_assert_int_eq(fgetc(output), EOF);
    fclose(output);

    // A terminal connected after failure receives the same ordered startup context.
    if (with_history) {
        const char* info = strstr(message, "I: startup context");
        const char* debug = strstr(message, "D: ROM read context");
        const char* warn = strstr(message, "W: timeout context");
        const char* error = strstr(message, diagnosis);
        ck_assert_ptr_nonnull(info);
        ck_assert_ptr_nonnull(debug);
        ck_assert_ptr_nonnull(warn);
        ck_assert_ptr_nonnull(error);
        ck_assert(info < debug && debug < warn && warn < error);
        ck_assert_uint_eq(log_entry_count(LOG_LEVEL_DEBUG), 3);
    } else {
        ck_assert_ptr_null(strstr(message, "Recent diagnostic log:"));
    }

    ck_assert(!delays[0].on);
    ck_assert_uint_eq(delays[0].duration, 600000);
    for (size_t repeat = 0; repeat < 2; ++repeat) {
        for (size_t index = 0; index < expected_count; ++index) {
            const size_t captured = repeat * expected_count + index;
            ck_assert_int_eq(delays[captured].on, index >= 2 && index % 2 == 0);
            ck_assert_uint_eq(delays[captured].duration, expected[index]);
        }
    }
    ck_assert(!delays[2 * expected_count].on);
    ck_assert_uint_eq(delays[2 * expected_count].duration, expected[0]);
}

// FPGA failures must report C without attempting to use an uninitialized display.
START_TEST(test_fpga_error_reporting) {
    const uint64_t expected[] = {
        600000, 100000, 300000, 100000, 100000, 100000, 300000, 100000, 100000,
    };
    check_fatal_reporting(FATAL_ERROR_FPGA_CONFIG, "Error (C):", expected, count_of(expected), false);
}
END_TEST

// Repeated E is a fatal error, unlike the bootloader's single entering-firmware report.
START_TEST(test_unspecified_error_reporting) {
    const uint64_t expected[] = {600000, 100000, 100000};
    check_fatal_reporting(FATAL_ERROR_UNSPECIFIED, "Error (E):", expected, count_of(expected), false);
}
END_TEST

// An invalid registry code must still terminate with a recognizable Morse error.
START_TEST(test_invalid_error_code) {
    const uint64_t expected[] = {600000, 100000, 100000};
    check_fatal_reporting((fatal_error_t)-1, "Error (E):", expected, count_of(expected), false);
}
END_TEST

// Replay all retained levels in each periodic report for late UART connections.
START_TEST(test_fatal_replays_log_history) {
    const uint64_t expected[] = {600000, 100000, 100000};
    check_fatal_reporting(FATAL_ERROR_UNSPECIFIED, "Error (E):", expected, count_of(expected), true);
}
END_TEST

// The longest retained message and a wide uptime must print without truncation.
START_TEST(test_fatal_log_line_capacity) {
    mock_clock_set(0);
    log_init();
    char payload[LOG_MESSAGE_LENGTH];
    memset(payload, 'x', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    mock_clock_set(UINT64_MAX - FATAL_LOG_DUMP_TEST_ELAPSED_US);
    log_debug("%s", payload);

    // Capture only initial reporting, before Morse timing or repeat policy takes effect.
    FILE* const output = tmpfile();
    ck_assert_ptr_nonnull(output);
    fflush(stdout);
    const int saved_stdout = dup(fileno(stdout));
    ck_assert_int_ge(saved_stdout, 0);
    ck_assert_int_ge(dup2(fileno(output), fileno(stdout)), 0);
    delay_count = stdio_init_count = 0;
    delay_limit = 1;
    observing = true;
    if (setjmp(fatal_exit) == 0) {
        __real_fatal_halt(FATAL_ERROR_UNSPECIFIED, "test fatal details");
    }
    observing = false;
    fflush(stdout);
    ck_assert_int_ge(dup2(saved_stdout, fileno(stdout)), 0);
    close(saved_stdout);

    // Include the newline in the comparison so a clipped message cannot pass.
    rewind(output);
    char message[256] = {0};
    ck_assert_uint_lt(fread(message, 1, sizeof(message) - 1, output), sizeof(message) - 1);
    fclose(output);
    const char* const line = strstr(message, "[18446744073698.551] D: ");
    ck_assert_ptr_nonnull(line);
    const char* const text = line + strlen("[18446744073698.551] D: ");
    ck_assert_int_eq(strncmp(text, payload, strlen(payload)), 0);
    ck_assert_int_eq(text[strlen(payload)], '\r');
    ck_assert_int_eq(text[strlen(payload) + 1], '\n');
}
END_TEST

// Verify that a boot failure uses the display-independent entry point.
static void check_halt_only(void) {
    ck_assert_msg(halt_only_called, "startup failure attempted normal fatal display reporting");
}

// An absent SD device must fail safely before the displays have been initialized.
START_TEST(test_sd_startup_failure) {
    halt_only_called = false;
    test_expect_fatal_message("sd activity requires a device with read and program callbacks");
    test_expect_fatal_callback(check_halt_only);
    sd_activity_install(NULL);
}
END_TEST

// Exercise the real ROM loader with mocked peripherals and synthetic ROM bytes.
static void check_rom_boot(menu_rom_boot_reason_t reason, unsigned int expected_keymap_reads) {
    mock_reset();
    mock_clock_set(time_us_64() + SD_ACTIVITY_QUIET_US);
    keymap_reads = model_reads = model_writes = 0;
    observing_rom_boot = true;
    start_menu_rom(reason);
    observing_rom_boot = false;

    ck_assert_uint_eq(keymap_reads, expected_keymap_reads);
    ck_assert_uint_eq(model_reads, 1);
    ck_assert_uint_eq(model_writes, 1);
    ck_assert_int_eq(mock_cpu_type(), CPU_SOFT_6502);
}

// Fatal rendering must work without requesting any SD keymap.
START_TEST(test_error_rom_without_keymap) {
    check_rom_boot(MENU_ROM_BOOT_ERROR, 0);
}
END_TEST

// Interactive menus must retain their existing USB keymap initialization.
START_TEST(test_normal_rom_with_keymap) {
    check_rom_boot(MENU_ROM_BOOT_NORMAL, 1);
}
END_TEST

// The real ROM reset and menu routing transition must retain held physical navigation.
START_TEST(test_menu_rom_preserves_physical_holds) {
    mock_reset();
    mock_clock_set(time_us_64() + SD_ACTIVITY_QUIET_US);
    rom_keyboard_model = _i / 2 == 0 ? pet_keyboard_model_graphics : pet_keyboard_model_business;
    system_state.pet_keyboard_model = rom_keyboard_model;
    const bool business = rom_keyboard_model == pet_keyboard_model_business;
    const key_event_t key = _i % 2 == 0
        ? (business ? PET_KEY_DOWN_B : PET_KEY_DOWN_N)
        : (business ? PET_KEY_RETURN_B : PET_KEY_RETURN_N);
    uint8_t physical[KEY_COL_COUNT];
    memset(physical, UINT8_MAX, sizeof(physical));
    physical[PET_KEY_COL(key)] &= (uint8_t)~(1u << PET_KEY_ROW(key));
    mock_input_pet_snapshot(physical);
    input_task();
    observing_rom_boot = true;
    start_menu_rom(MENU_ROM_BOOT_NORMAL);
    observing_rom_boot = false;
    input_routing_set_menu_active(true);
    input_task();
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);

    // Only a genuinely observed release and repress can navigate the new menu.
    physical[PET_KEY_COL(key)] |= 1u << PET_KEY_ROW(key);
    mock_input_pet_snapshot(physical);
    input_task();
    physical[PET_KEY_COL(key)] &= (uint8_t)~(1u << PET_KEY_ROW(key));
    mock_input_pet_snapshot(physical);
    input_task();
    ck_assert_int_eq(input_routing_navigation(), _i % 2 == 0 ? tui_action_down : tui_action_activate);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
}
END_TEST

Suite* fatal_error_suite(void) {
    Suite* const suite = suite_create("Morse error diagnostics");
    TCase* const tests = tcase_create("fatal reporting");
    tcase_add_test(tests, test_fpga_error_reporting);
    tcase_add_test(tests, test_unspecified_error_reporting);
    tcase_add_test(tests, test_invalid_error_code);
    tcase_add_test(tests, test_fatal_replays_log_history);
    tcase_add_test_raise_signal(tests, test_sd_startup_failure, SIGABRT);
    tcase_add_test(tests, test_error_rom_without_keymap);
    tcase_add_test(tests, test_normal_rom_with_keymap);
    tcase_add_loop_test(tests, test_menu_rom_preserves_physical_holds, 0, 4);
    suite_add_tcase(suite, tests);
    TCase* const formatting = tcase_create("fatal log formatting");
    tcase_add_test(formatting, test_fatal_log_line_capacity);
    suite_add_tcase(suite, formatting);
    return suite;
}
