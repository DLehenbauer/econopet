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

    bool ok = bp_set(0x0400);
    ck_assert(ok);
    ck_assert_int_eq(bp_count(), 1);

    // SRAM should now contain STP ($DB).
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);
} END_TEST

START_TEST(test_bp_set_duplicate_fails) {
    mock_reset();
    ck_assert(bp_set(0x0400));
    ck_assert(!bp_set(0x0400));  // Duplicate
    ck_assert_int_eq(bp_count(), 1);
} END_TEST

START_TEST(test_bp_set_table_full) {
    mock_reset();
    for (int i = 0; i < BP_MAX; i++) {
        ck_assert(bp_set((uint16_t)(0x1000 + i)));
    }
    ck_assert_int_eq(bp_count(), BP_MAX);

    // One more should fail.
    ck_assert(!bp_set(0x2000));
    ck_assert_int_eq(bp_count(), BP_MAX);
} END_TEST

START_TEST(test_bp_remove_restores_sram) {
    mock_reset();
    mock_ram[0x0400] = 0xA9;

    bp_set(0x0400);
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
    bp_set(0x0100);
    bp_set(0x0200);
    bp_set(0x0300);
    ck_assert_int_eq(bp_count(), 3);

    // Remove the middle entry.
    ck_assert(bp_remove(0x0200));
    ck_assert_int_eq(bp_count(), 2);

    // The remaining entries should still be removable.
    ck_assert(bp_remove(0x0100));
    ck_assert(bp_remove(0x0300));
    ck_assert_int_eq(bp_count(), 0);
} END_TEST

START_TEST(test_bp_resume_restores_and_rearms) {
    mock_reset();
    mock_ram[0x0400] = 0x4C;  // JMP abs

    bp_set(0x0400);
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);

    // Simulate FPGA halt at $0400.
    system_state.bp_halted = true;
    mock_bp_addr   = 0x0400;
    mock_bp_cleared = false;

    uint16_t pc = bp_resume();
    ck_assert_uint_eq(pc, 0x0400);

    // The halt should have been cleared.
    ck_assert(mock_bp_cleared);

    // After resume the breakpoint should be re-armed (STP written back).
    ck_assert_uint_eq(mock_ram[0x0400], 0xDB);

    // The entry should still be in the table.
    ck_assert_int_eq(bp_count(), 1);
} END_TEST

START_TEST(test_bp_resume_unknown_addr) {
    mock_reset();

    // No breakpoints set, but FPGA halted (e.g., user program contained STP).
    system_state.bp_halted = true;
    mock_bp_addr   = 0x0800;
    mock_bp_cleared = false;

    uint16_t pc = bp_resume();
    ck_assert_uint_eq(pc, 0x0800);

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

    bp_set(0x0300);
    bp_set(0x0400);

    // Remove them and check originals are restored.
    bp_remove(0x0300);
    ck_assert_uint_eq(mock_ram[0x0300], 0x60);

    bp_remove(0x0400);
    ck_assert_uint_eq(mock_ram[0x0400], 0xEA);
} END_TEST

// ---------------------------------------------------------------------------
// Suite
// ---------------------------------------------------------------------------

Suite *breakpoint_suite(void) {
    Suite *s = suite_create("breakpoint");

    TCase *tc = tcase_create("core");
    tcase_add_test(tc, test_bp_init_clears_table);
    tcase_add_test(tc, test_bp_set_patches_sram);
    tcase_add_test(tc, test_bp_set_duplicate_fails);
    tcase_add_test(tc, test_bp_set_table_full);
    tcase_add_test(tc, test_bp_remove_restores_sram);
    tcase_add_test(tc, test_bp_remove_nonexistent_fails);
    tcase_add_test(tc, test_bp_remove_compacts_table);
    tcase_add_test(tc, test_bp_resume_restores_and_rearms);
    tcase_add_test(tc, test_bp_resume_unknown_addr);
    tcase_add_test(tc, test_bp_set_saves_original);
    suite_add_tcase(s, tc);

    return s;
}
