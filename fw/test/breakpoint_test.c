// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "breakpoint_test.h"

#include <string.h>

#include "breakpoint.h"
#include "diag/log/log.h"
#include "driver.h"

// ---------------------------------------------------------------------------
// Mock SRAM and FPGA state used by the driver stubs below.
// ---------------------------------------------------------------------------

#define MOCK_RAM_SIZE 0x10000
static uint8_t mock_ram[MOCK_RAM_SIZE];

static uint16_t mock_bp_addr;
static bool     mock_bp_cleared;

// Track the last spi_write_at call for assertions.
static uint32_t last_write_addr;
static uint8_t  last_write_data;

// ---------------------------------------------------------------------------
// Driver stubs (these replace the real SPI functions during testing).
// ---------------------------------------------------------------------------

uint8_t spi_read_at(uint32_t addr) {
    ck_assert_uint_lt(addr, MOCK_RAM_SIZE);
    return mock_ram[addr];
}

uint8_t spi_write_at(uint32_t addr, uint8_t data) {
    last_write_addr = addr;
    last_write_data = data;
    if (addr < MOCK_RAM_SIZE) {
        mock_ram[addr] = data;
    }
    return 0;
}

void spi_write(uint32_t addr, const uint8_t* pSrc, size_t byteLength) {
    for (size_t i = 0; i < byteLength; i++) {
        if (addr + i < MOCK_RAM_SIZE) {
            mock_ram[addr + i] = pSrc[i];
        }
    }
}

uint8_t spi_read_next(void) {
    return 0;
}

uint8_t spi_read_prev(void) {
    return 0;
}

uint16_t bp_hit_addr(void) {
    return mock_bp_addr;
}

void bp_clear_halt(void) {
    mock_bp_cleared = true;
    system_state.bp_halted = false;
}

void sleep_us(uint64_t us) {
    (void)us;
}

// ---------------------------------------------------------------------------
// Helper to reset mock state before each test.
// ---------------------------------------------------------------------------

static void mock_reset(void) {
    memset(mock_ram, 0xEA, sizeof(mock_ram));  // Fill with NOP
    system_state.bp_halted = false;
    mock_bp_addr    = 0;
    mock_bp_cleared = false;
    last_write_addr = 0;
    last_write_data = 0;
    log_init();
    bp_init();
}

// Default callback: resume at pc with re-arm.
static bp_result_t default_callback(uint16_t pc, void* context) {
    (void)context;
    return (bp_result_t){ .pc = pc, .rearm = true };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

START_TEST(test_bp_init_clears_table) {
    mock_reset();
    ck_assert_int_eq(bp_count(), 0);
} END_TEST

START_TEST(test_bp_set_patches_sram) {
    mock_reset();
    mock_ram[0x0400] = 0xA9;  // LDA #imm

    bp_set(0x0400, default_callback, NULL);
    ck_assert_int_eq(bp_count(), 1);

    // SRAM should now contain STP ($DB).
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);
} END_TEST

START_TEST(test_bp_remove_restores_sram) {
    mock_reset();
    mock_ram[0x0400] = 0xA9;

    bp_set(0x0400, default_callback, NULL);
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);

    bool ok = bp_remove(0x0400);
    ck_assert(ok);
    ck_assert_int_eq(bp_count(), 0);

    // Original byte should be restored.
    ck_assert_uint_eq(mock_ram[0x0400], 0xA9);
} END_TEST

START_TEST(test_bp_remove_nonexistent_fails) {
    mock_reset();
    ck_assert(!bp_remove(0x0400));
} END_TEST

START_TEST(test_bp_remove_compacts_table) {
    mock_reset();
    bp_set(0x0100, default_callback, NULL);
    bp_set(0x0200, default_callback, NULL);
    bp_set(0x0300, default_callback, NULL);
    ck_assert_int_eq(bp_count(), 3);

    // Remove the middle entry.
    ck_assert(bp_remove(0x0200));
    ck_assert_int_eq(bp_count(), 2);

    // The remaining entries should still be removable.
    ck_assert(bp_remove(0x0100));
    ck_assert(bp_remove(0x0300));
    ck_assert_int_eq(bp_count(), 0);
} END_TEST

START_TEST(test_bp_task_restores_and_rearms) {
    mock_reset();
    mock_ram[0x0400] = 0x4C;  // JMP abs

    bp_set(0x0400, default_callback, NULL);
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);

    // Simulate FPGA halt at $0400.
    system_state.bp_halted = true;
    mock_bp_addr   = 0x0400;
    mock_bp_cleared = false;

    bp_task();

    // The halt should have been cleared.
    ck_assert(mock_bp_cleared);

    // After resume the breakpoint should be re-armed (STP written back).
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);

    // The entry should still be in the table.
    ck_assert_int_eq(bp_count(), 1);
} END_TEST

START_TEST(test_bp_task_unknown_addr) {
    mock_reset();

    // No breakpoints set, but FPGA halted (e.g., user program contained STP).
    system_state.bp_halted = true;
    mock_bp_addr   = 0x0800;
    mock_bp_cleared = false;

    bp_task();

    // Halt should still be cleared.
    ck_assert(mock_bp_cleared);

    // Table should remain empty.
    ck_assert_int_eq(bp_count(), 0);
} END_TEST

START_TEST(test_bp_set_saves_original) {
    mock_reset();

    // Set different values at two addresses.
    mock_ram[0x0300] = 0x60;  // RTS
    mock_ram[0x0400] = 0xEA;  // NOP

    bp_set(0x0300, default_callback, NULL);
    bp_set(0x0400, default_callback, NULL);

    // Remove them and check originals are restored.
    bp_remove(0x0300);
    ck_assert_uint_eq(mock_ram[0x0300], 0x60);

    bp_remove(0x0400);
    ck_assert_uint_eq(mock_ram[0x0400], 0xEA);
} END_TEST

// Callback state for testing
static bool callback_invoked;
static uint16_t callback_addr;
static void* callback_context;
static uint16_t callback_resume_addr;  // Address to resume at
static bool callback_rearm;            // Whether to re-arm breakpoint

static bp_result_t test_callback(uint16_t pc, void* context) {
    callback_invoked = true;
    callback_addr = pc;
    callback_context = context;
    return (bp_result_t){ .pc = callback_resume_addr, .rearm = callback_rearm };
}

START_TEST(test_bp_callback_auto_resume) {
    mock_reset();
    mock_ram[0x0400] = 0x4C;  // JMP abs

    // Reset callback state
    callback_invoked = false;
    callback_addr = 0;
    callback_context = NULL;
    callback_resume_addr = 0x0400;  // Resume at pc (normal)
    callback_rearm = true;

    int test_context = 42;
    bp_set(0x0400, test_callback, &test_context);
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);

    // Simulate FPGA halt at $0400.
    system_state.bp_halted = true;
    mock_bp_addr = 0x0400;
    mock_bp_cleared = false;

    // Run bp_task which should invoke the callback and auto-resume.
    bp_task();

    // Verify callback was invoked with the correct address and context.
    ck_assert(callback_invoked);
    ck_assert_uint_eq(callback_addr, 0x0400);
    ck_assert_ptr_eq(callback_context, &test_context);

    // The halt should have been cleared (auto-resume happened).
    ck_assert(mock_bp_cleared);

    // Breakpoint should be re-armed.
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);
} END_TEST

START_TEST(test_bp_callback_skip_one_byte) {
    mock_reset();
    mock_ram[0x0400] = 0xA9;  // Original: LDA #imm
    mock_ram[0x0401] = 0x12;
    mock_ram[0x0402] = 0x34;

    // Reset callback state
    callback_invoked = false;
    callback_resume_addr = 0x0401;  // Skip 1 byte (NOP)
    callback_rearm = true;

    bp_set(0x0400, test_callback, NULL);
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);

    // Simulate FPGA halt at $0400.
    system_state.bp_halted = true;
    mock_bp_addr = 0x0400;
    mock_bp_cleared = false;

    bp_task();

    ck_assert(callback_invoked);
    ck_assert(mock_bp_cleared);

    // After re-arm, STP should be back at $0400, original byte restored
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);
} END_TEST

START_TEST(test_bp_callback_skip_two_bytes) {
    mock_reset();
    mock_ram[0x0400] = 0xA9;  // Original bytes
    mock_ram[0x0401] = 0x12;
    mock_ram[0x0402] = 0x34;

    // Reset callback state
    callback_invoked = false;
    callback_resume_addr = 0x0402;  // Skip 2 bytes (NOP NOP)
    callback_rearm = true;

    bp_set(0x0400, test_callback, NULL);

    // Simulate FPGA halt at $0400.
    system_state.bp_halted = true;
    mock_bp_addr = 0x0400;
    mock_bp_cleared = false;

    bp_task();

    ck_assert(callback_invoked);
    ck_assert(mock_bp_cleared);

    // Originals should be restored, STP re-armed
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);
    ck_assert_uint_eq(mock_ram[0x0401], 0x12);
} END_TEST

START_TEST(test_bp_callback_redirect_jmp) {
    mock_reset();
    mock_ram[0x0400] = 0xA9;  // Original bytes
    mock_ram[0x0401] = 0x12;
    mock_ram[0x0402] = 0x34;

    // Reset callback state
    callback_invoked = false;
    callback_resume_addr = 0x9000;  // Redirect far away (JMP)
    callback_rearm = true;

    bp_set(0x0400, test_callback, NULL);

    // Simulate FPGA halt at $0400.
    system_state.bp_halted = true;
    mock_bp_addr = 0x0400;
    mock_bp_cleared = false;

    bp_task();

    ck_assert(callback_invoked);
    ck_assert(mock_bp_cleared);

    // Originals should be restored, STP re-armed
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);
    ck_assert_uint_eq(mock_ram[0x0401], 0x12);
    ck_assert_uint_eq(mock_ram[0x0402], 0x34);
} END_TEST

START_TEST(test_bp_callback_oneshot) {
    mock_reset();
    mock_ram[0x0400] = 0x4C;  // JMP abs

    // Reset callback state
    callback_invoked = false;
    callback_resume_addr = 0x0400;  // Resume at pc
    callback_rearm = false;         // One-shot: remove after resume

    bp_set(0x0400, test_callback, NULL);
    ck_assert_int_eq(bp_count(), 1);

    // Simulate FPGA halt at $0400.
    system_state.bp_halted = true;
    mock_bp_addr = 0x0400;
    mock_bp_cleared = false;

    bp_task();

    ck_assert(callback_invoked);
    ck_assert(mock_bp_cleared);

    // Original bytes should be restored (no STP).
    ck_assert_uint_eq(mock_ram[0x0400], 0x4C);

    // Breakpoint should have been removed from the table.
    ck_assert_int_eq(bp_count(), 0);
} END_TEST

// ---------------------------------------------------------------------------
// Suite
// ---------------------------------------------------------------------------

Suite *breakpoint_suite(void) {
    Suite *s = suite_create("breakpoint");

    TCase *tc = tcase_create("core");
    tcase_add_test(tc, test_bp_init_clears_table);
    tcase_add_test(tc, test_bp_set_patches_sram);
    tcase_add_test(tc, test_bp_remove_restores_sram);
    tcase_add_test(tc, test_bp_remove_nonexistent_fails);
    tcase_add_test(tc, test_bp_remove_compacts_table);
    tcase_add_test(tc, test_bp_task_restores_and_rearms);
    tcase_add_test(tc, test_bp_task_unknown_addr);
    tcase_add_test(tc, test_bp_set_saves_original);
    tcase_add_test(tc, test_bp_callback_auto_resume);
    tcase_add_test(tc, test_bp_callback_skip_one_byte);
    tcase_add_test(tc, test_bp_callback_skip_two_bytes);
    tcase_add_test(tc, test_bp_callback_redirect_jmp);
    tcase_add_test(tc, test_bp_callback_oneshot);
    suite_add_tcase(s, tc);

    return s;
}
