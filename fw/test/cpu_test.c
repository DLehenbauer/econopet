// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "cpu_test.h"

#include "breakpoint.h"
#include "cpu.h"
#include "diag/log/log.h"
#include "driver.h"
#include "mock.h"

#define TEST_RESET_VECTOR 0xFFFCu
#define TEST_RESULT_ADDRESS 0x0410u
#define TEST_LOOP_ADDRESS 0x040Au
#define TEST_UNRELATED_ADDRESS 0x0600u
#define TEST_MARKER 0xA5u
#define TEST_PROBE_BUDGET 16u
#define TEST_CLOCK_STEP_US 1000u

static bool omit_store;
static unsigned int probe_runs;
static bool unrelated_dispatched;

// Start with an uninitialized CPU module in this test's forked process.
static void reset_cpu_test(void) {
    mock_reset();
    log_init();
    bp_init();
    mock_clock_set(0);
    omit_store = false;
    probe_runs = 0;
    unrelated_dispatched = false;
}

// Read little-endian operands without sharing the production probe's layout.
static uint16_t read_address(uint16_t address) { return mock_ram[address] | ((uint16_t)mock_ram[address + 1] << 8); }

// Execute just the probe's small instruction subset through its installed STP.
static void execute_probe(void) {
    ck_assert_int_eq(mock_cpu_type(), CPU_PHYS_6502);
    ck_assert_uint_eq(mock_ram[TEST_RESULT_ADDRESS], 0);
    uint16_t pc = read_address(TEST_RESET_VECTOR);
    uint8_t accumulator = 0;
    ++probe_runs;

    for (unsigned int step = 0; step < TEST_PROBE_BUDGET; ++step) {
        switch (mock_ram[pc++]) {
        case 0x78: // SEI
        case 0xD8: // CLD
            break;
        case 0x4C: // JMP abs
            pc = read_address(pc);
            break;
        case 0xA9: // LDA #imm
            accumulator = mock_ram[pc++];
            break;
        case 0x8D: // STA abs
            if (!omit_store) {
                mock_ram[read_address(pc)] = accumulator;
            }
            pc += 2;
            break;
        case 0xDB: // STP fetch latched by the FPGA
            mock_breakpoint_set_hit_addr(pc - 1);
            system_state.bp_halted = true;
            return;
        default:
            ck_abort_msg("Unexpected probe opcode at $%04X", pc - 1);
        }
    }
    ck_abort_msg("Probe did not reach its breakpoint");
}

// Model an empty socket by advancing time without any CPU writes or fetches.
static void empty_socket(void) {
    ck_assert_int_eq(mock_cpu_type(), CPU_PHYS_6502);
    ck_assert_uint_eq(mock_ram[TEST_RESULT_ADDRESS], 0);
    ++probe_runs;
    mock_clock_advance(TEST_CLOCK_STEP_US);
}

START_TEST(test_cpu_init_detects_marker_and_query_is_read_only) {
    reset_cpu_test();
    mock_ram[TEST_RESULT_ADDRESS] = TEST_MARKER;
    mock_cpu_set_run_callback(execute_probe);

    cpu_init();

    ck_assert(physical_cpu_present());
    ck_assert_uint_eq(mock_ram[TEST_RESULT_ADDRESS], TEST_MARKER);
    ck_assert_int_eq(mock_cpu_type(), CPU_PHYS_6502);
    ck_assert_int_eq(get_cpu(), CPU_RESET);
    ck_assert_int_eq(bp_count(), 0);
    ck_assert(!system_state.bp_halted);
    ck_assert_uint_eq(probe_runs, 1);

    // Queries after a different core has been selected must not touch routing.
    set_cpu_type(CPU_SOFT_6809);
    const uint64_t before_query = time_us_64();
    ck_assert(physical_cpu_present());
    ck_assert(physical_cpu_present());
    ck_assert_int_eq(mock_cpu_type(), CPU_SOFT_6809);
    ck_assert_uint_eq(probe_runs, 1);
    ck_assert_uint_eq(time_us_64(), before_query);
}
END_TEST

START_TEST(test_cpu_init_empty_socket_selects_soft_6502) {
    reset_cpu_test();
    mock_ram[TEST_RESULT_ADDRESS] = TEST_MARKER;
    // A retained matching halt must not substitute for fresh CPU execution.
    mock_breakpoint_set_hit_addr(TEST_LOOP_ADDRESS);
    system_state.bp_halted = true;
    mock_cpu_set_run_callback(empty_socket);

    cpu_init();

    ck_assert(!physical_cpu_present());
    ck_assert_int_eq(mock_cpu_type(), CPU_SOFT_6502);
    ck_assert_int_eq(get_cpu(), CPU_RESET);
    ck_assert_int_eq(bp_count(), 0);
    ck_assert(!system_state.bp_halted);
    ck_assert_uint_gt(probe_runs, 0);
    ck_assert_uint_eq(mock_ram[TEST_LOOP_ADDRESS], 0x4C);
}
END_TEST

// Record any accidental dispatch of a breakpoint outside the startup probe.
static bp_result_t unrelated_callback(uint16_t pc, void* context) {
    (void)context;
    unrelated_dispatched = true;
    return (bp_result_t){.pc = pc, .rearm = true};
}

// Latch an unrelated hit without executing the marker-writing program.
static void unrelated_fetch(void) {
    mock_breakpoint_set_hit_addr(TEST_UNRELATED_ADDRESS);
    system_state.bp_halted = true;
}

START_TEST(test_cpu_init_unrelated_halt_preserves_other_breakpoint) {
    reset_cpu_test();
    bp_set(TEST_UNRELATED_ADDRESS, unrelated_callback, NULL);
    mock_cpu_set_run_callback(unrelated_fetch);

    cpu_init();

    ck_assert(!physical_cpu_present());
    ck_assert(!unrelated_dispatched);
    ck_assert_int_eq(get_cpu(), CPU_RESET);
    ck_assert_int_eq(mock_cpu_type(), CPU_SOFT_6502);
    ck_assert(!system_state.bp_halted);
    ck_assert_uint_eq(mock_ram[TEST_LOOP_ADDRESS], 0x4C);
    ck_assert_int_eq(bp_count(), 1);
    ck_assert_uint_eq(bp_get(0)->addr, TEST_UNRELATED_ADDRESS);
    ck_assert_uint_eq(mock_ram[TEST_UNRELATED_ADDRESS], 0xDB);
}
END_TEST

// A bad marker must reach fatal reporting only after the probe has been removed.
static void check_failed_probe_cleanup(void) {
    ck_assert_int_eq(get_cpu(), CPU_RESET);
    ck_assert_int_eq(bp_count(), 0);
    ck_assert(!system_state.bp_halted);
    ck_assert_uint_eq(mock_ram[TEST_LOOP_ADDRESS], 0x4C);
}

START_TEST(test_cpu_init_marker_mismatch_is_fatal) {
    reset_cpu_test();
    mock_ram[TEST_RESULT_ADDRESS] = TEST_MARKER;
    omit_store = true;
    mock_cpu_set_run_callback(execute_probe);
    test_expect_fatal_message("probe marker mismatch");
    test_expect_fatal_callback(check_failed_probe_cleanup);

    cpu_init();
}
END_TEST

START_TEST(test_cpu_query_requires_init) {
    reset_cpu_test();
    test_expect_fatal_message("cpu_init required");

    physical_cpu_present();
}
END_TEST

START_TEST(test_cpu_init_rejects_repeated_initialization) {
    reset_cpu_test();
    mock_cpu_set_run_callback(execute_probe);
    cpu_init();
    test_expect_fatal_message("already initialized");

    cpu_init();
}
END_TEST

START_TEST(test_cpu_init_rejects_running_cpu) {
    reset_cpu_test();
    set_cpu(CPU_READY);
    test_expect_fatal_message("CPU must be reset or halted");

    cpu_init();
}
END_TEST

Suite* cpu_suite(void) {
    Suite* suite = suite_create("cpu");
    TCase* tests = tcase_create("startup");
    tcase_add_test(tests, test_cpu_init_detects_marker_and_query_is_read_only);
    tcase_add_test(tests, test_cpu_init_empty_socket_selects_soft_6502);
    tcase_add_test(tests, test_cpu_init_unrelated_halt_preserves_other_breakpoint);
    tcase_add_test_raise_signal(tests, test_cpu_init_marker_mismatch_is_fatal, SIGABRT);
    tcase_add_test_raise_signal(tests, test_cpu_query_requires_init, SIGABRT);
    tcase_add_test_raise_signal(tests, test_cpu_init_rejects_repeated_initialization, SIGABRT);
    tcase_add_test_raise_signal(tests, test_cpu_init_rejects_running_cpu, SIGABRT);
    suite_add_tcase(suite, tests);
    return suite;
}
