// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "roms_test.h"

#include <signal.h>

#include "diag/log/log.h"
#include "driver.h"
#include "mock.h"
#include "roms/roms.h"
#include "system/state.h"

static unsigned int rom_wait_services;
static system_state_t saved_state;

static void setup(void) {
    log_init();
    saved_state = system_state;
}

static void teardown(void) {
    system_state = saved_state;
}

/** Assert the ROM reset releases any retained breakpoint before execution. */
static void check_rom_reset_release(cpu_state_t state) {
    if (state == CPU_READY) {
        ck_assert_int_eq(get_cpu(), CPU_RESET);
        ck_assert(mock_breakpoint_halt_was_cleared());
        ck_assert(!system_state.bp_halted);
    }
}

/** Verify diagnostic ROM reset and fixed display policy. */
static void check_rom_boot(menu_rom_boot_reason_t reason) {
    mock_reset();
    mock_clock_set(time_us_64());
    system_state.bp_halted = true;
    mock_cpu_set_state_observer(check_rom_reset_release);
    start_menu_rom(reason);
    mock_cpu_set_state_observer(NULL);

    ck_assert_int_eq(mock_cpu_type(), CPU_SOFT_6502);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_40);
    ck_assert_uint_eq(system_state.video_ram_mask, pet_video_ram_mask_1kb);
    ck_assert(mock_breakpoint_halt_was_cleared());
    ck_assert(!system_state.bp_halted);
    ck_assert_int_eq(get_cpu(), CPU_READY);
}

/** Fatal rendering must not request an SD keymap. */
START_TEST(test_error_rom_reset) {
    check_rom_boot(MENU_ROM_BOOT_ERROR);
}
END_TEST

/** Interactive menus retain USB keymap initialization. */
START_TEST(test_normal_rom_reset) {
    check_rom_boot(MENU_ROM_BOOT_NORMAL);
}
END_TEST

/** Publish completion after one service, modeling the diagnostic CPU's store. */
static void complete_diagnostic_rom(void) {
    ++rom_wait_services;
    spi_write_at(MENU_ROM_COMPLETION_ADDRESS, MENU_ROM_COMPLETION_VALUE);
}

/** Advance beyond the bounded wait without publishing completion. */
static void stall_diagnostic_rom(void) {
    ++rom_wait_services;
    mock_clock_advance(3000000u);
}

/** Preserve timeout diagnostics before the fatal terminal path takes over. */
static void check_rom_timeout_log(void) {
    ck_assert_uint_eq(rom_wait_services, 1);
    log_iterator_t iter;
    log_iter_init(&iter, LOG_LEVEL_WARN);
    const log_entry_t* entry = log_iter_next(&iter, NULL);
    ck_assert_ptr_nonnull(entry);
    ck_assert_str_eq(entry->message, "ROM timeout: marker=00");
    entry = log_iter_next(&iter, NULL);
    ck_assert_ptr_nonnull(entry);
    ck_assert_ptr_nonnull(strstr(entry->message, "ROM CPU="));
    ck_assert_ptr_null(strstr(entry->message, "suspend="));
}

/** Accept a previously completed marker without servicing it again. */
START_TEST(test_rom_wait_completion) {
    mock_reset();
    rom_wait_services = 0;
    spi_write_at(MENU_ROM_COMPLETION_ADDRESS, 0);
    menu_rom_wait(complete_diagnostic_rom);
    ck_assert_uint_eq(rom_wait_services, 1);
    menu_rom_wait(complete_diagnostic_rom);
    ck_assert_uint_eq(rom_wait_services, 1);
}
END_TEST

/** Record timeout evidence through the real wait algorithm. */
START_TEST(test_rom_wait_timeout_diagnostics) {
    mock_reset();
    mock_clock_set(time_us_64());
    log_init();
    rom_wait_services = 0;
    spi_write_at(MENU_ROM_COMPLETION_ADDRESS, 0);
    test_expect_fatal_message("diagnostic ROM did not complete (marker $00)");
    test_expect_fatal_callback(check_rom_timeout_log);
    menu_rom_wait(stall_diagnostic_rom);
}
END_TEST

Suite* roms_suite(void) {
    Suite* const suite = suite_create("roms");
    TCase* const tests = tcase_create("loader");
    tcase_add_checked_fixture(tests, setup, teardown);
    tcase_add_test(tests, test_error_rom_reset);
    tcase_add_test(tests, test_normal_rom_reset);
    tcase_add_test(tests, test_rom_wait_completion);
    suite_add_tcase(suite, tests);
    return suite;
}

Suite* roms_fatal_suite(void) {
    Suite* const suite = suite_create("roms-fatal");
    TCase* const tests = tcase_create("timeout");
    tcase_add_checked_fixture(tests, setup, teardown);
    tcase_add_test_raise_signal(
        tests, test_rom_wait_timeout_diagnostics, SIGABRT);
    suite_add_tcase(suite, tests);
    return suite;
}
