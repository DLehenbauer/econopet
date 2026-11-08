// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "fatal_error_test.h"

#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hardware/gpio.h"

#include "fatal.h"
#include "mock.h"
#include "roms/roms.h"
#include "sd/activity.h"

static jmp_buf fatal_exit;
static bool observing;
static bool led_on;
static bool led_initialized;
static bool led_output;
static bool halt_only_called;
static bool observing_rom_boot;
static unsigned int keymap_reads;
static unsigned int model_reads;
static unsigned int model_writes;
static size_t stdio_init_count;
static size_t delay_count;
static size_t delay_limit;
static size_t pattern_delay_count;
static off_t first_message_size;
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
    state->pet_keyboard_model = pet_keyboard_model_graphics;
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
    // Check emitted bytes before the test's final flush can hide a missing flush.
    if (delay_count == 0 || delay_count == 1 + pattern_delay_count) {
        struct stat status;
        ck_assert_int_eq(fstat(fileno(stdout), &status), 0);
        if (delay_count == 0) {
            ck_assert_int_gt(status.st_size, 0);
            first_message_size = status.st_size;
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

// Capture serial output and two Morse repetitions through the shared fatal endpoint.
static void check_fatal_reporting(fatal_error_t error, const char* diagnosis,
                                  const uint64_t* expected, size_t expected_count) {
    FILE* const output = tmpfile();
    ck_assert_ptr_nonnull(output);
    fflush(stdout);
    const int saved_stdout = dup(fileno(stdout));
    ck_assert_int_ge(saved_stdout, 0);
    ck_assert_int_ge(dup2(fileno(output), fileno(stdout)), 0);

    delay_count = 0;
    stdio_init_count = 0;
    pattern_delay_count = expected_count;
    delay_limit = 1 + 2 * expected_count;
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
    char message[256];
    ck_assert_ptr_nonnull(fgets(message, sizeof(message), output));
    ck_assert_ptr_nonnull(strstr(message, diagnosis));
    ck_assert_ptr_nonnull(strstr(message, "test fatal details"));
    char repeated_message[sizeof(message)];
    ck_assert_ptr_nonnull(fgets(repeated_message, sizeof(repeated_message), output));
    ck_assert_str_eq(repeated_message, message);
    ck_assert_int_eq(fgetc(output), EOF);
    fclose(output);

    ck_assert(!delays[0].on);
    ck_assert_uint_eq(delays[0].duration, 300000);
    for (size_t repeat = 0; repeat < 2; ++repeat) {
        for (size_t index = 0; index < expected_count; ++index) {
            const size_t captured = 1 + repeat * expected_count + index;
            ck_assert_int_eq(delays[captured].on, index < expected_count - 1 && index % 2 == 0);
            ck_assert_uint_eq(delays[captured].duration, expected[index]);
        }
    }
}

// FPGA failures must report C without attempting to use an uninitialized display.
START_TEST(test_fpga_error_reporting) {
    const uint64_t expected[] = {
        300000, 100000, 100000, 100000, 300000, 100000, 100000, 100000, 300000,
    };
    check_fatal_reporting(FATAL_ERROR_FPGA_CONFIG, "Error (C):",
                          expected, count_of(expected));
}
END_TEST

// Repeated E is a fatal error, unlike the bootloader's single entering-firmware report.
START_TEST(test_unspecified_error_reporting) {
    const uint64_t expected[] = {100000, 100000, 300000};
    check_fatal_reporting(FATAL_ERROR_UNSPECIFIED, "Error (E):",
                          expected, count_of(expected));
}
END_TEST

// An invalid registry code must still terminate with a recognizable Morse error.
START_TEST(test_invalid_error_code) {
    const uint64_t expected[] = {100000, 100000, 300000};
    check_fatal_reporting((fatal_error_t)-1, "Error (E):",
                          expected, count_of(expected));
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

Suite* fatal_error_suite(void) {
    Suite* const suite = suite_create("Morse error diagnostics");
    TCase* const tests = tcase_create("fatal reporting");
    tcase_add_test(tests, test_fpga_error_reporting);
    tcase_add_test(tests, test_unspecified_error_reporting);
    tcase_add_test(tests, test_invalid_error_code);
    tcase_add_test_raise_signal(tests, test_sd_startup_failure, SIGABRT);
    tcase_add_test(tests, test_error_rom_without_keymap);
    tcase_add_test(tests, test_normal_rom_with_keymap);
    suite_add_tcase(suite, tests);
    return suite;
}
