// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag_test.h"

#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

#include "cpu.h"
#include "diag/diag.h"
#include "diag/drive.h"
#include "diag/log/log.h"
#include "diag/mem.h"
#include "diag/sd.h"
#include "diag/target.h"
#include "display/char_encoding.h"
#include "display/display.h"
#include "display/dvi/crtc.h"
#include "drive-test/drive_test_catalog.h"
#include "ieee/ieee_drive.h"
#include "input/input_uart.h"
#include "menu/menu_browser.h"
#include "mock.h"
#include "roms/roms.h"
#include "system/system.h"
#include "ui/cli.h"

static system_state_t saved_state;
static diag_session_t session;
static unsigned calls[4];
static size_t next_call;
static bool inject_failure;
static unsigned scroll_calls;
static FILE* capture;
static int saved_stdout;
static jmp_buf diagnostic_escape;
static bool diagnostic_observed;
static bool swap_prepare_result;
static unsigned swap_prepare_calls;
static bool reset_observed;
static bool complete_observed;
static uint8_t observed_max_cycles;
static unsigned prompt_step;
static unsigned boot_video_calls;
typedef enum {
    PROMPT_ABORT,
    PROMPT_ESCAPE_ABORT,
    PROMPT_LAUNCH,
    PROMPT_COUNTDOWN_ABORT,
    PROMPT_COUNTDOWN_REMOVE,
    PROMPT_BOUNCE,
    PROMPT_INITIAL_ABSENT,
} prompt_script_t;
static prompt_script_t prompt_script;

/** Observe storage takeover without executing the separate SD fixture. */
static bool observe_swap_prepare(void) {
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ++swap_prepare_calls;
    ck_assert(menu_browser_abandon_scan());
    return swap_prepare_result;
}

/** Escape the real system reset boundary rather than resuming the interrupted menu. */
static void observe_reset(void) {
    reset_observed = true;
    longjmp(diagnostic_escape, 1);
}

/** Drive card contacts and UART while the production confirmation loop owns input. */
static void drive_prompt(uint64_t us) {
    (void)us;
    mock_clock_advance(50000);
    ++prompt_step;
    ck_assert_msg(prompt_step < 20, "SD prompt did not terminate");
    if (prompt_script == PROMPT_ABORT) {
        mock_input_uart_bytes((const uint8_t*)" ", 1);
        return;
    }
    if (prompt_script == PROMPT_ESCAPE_ABORT) {
        mock_input_uart_bytes((const uint8_t*)"\x1b", 1);
        return;
    }
    if (prompt_script == PROMPT_BOUNCE) {
        if (prompt_step == 1 || prompt_step == 4 || prompt_step == 7) mock_sd_card_present(false);
        if (prompt_step == 2 || prompt_step == 6) mock_sd_card_present(true);
        if (prompt_step == 9) mock_input_uart_bytes((const uint8_t*)" ", 1);
        return;
    }
    if (prompt_script == PROMPT_INITIAL_ABSENT) {
        if (prompt_step == 2) mock_sd_card_present(true);
        if (prompt_step == 4) mock_input_uart_bytes((const uint8_t*)" ", 1);
        return;
    }
    if (prompt_step == 1) mock_sd_card_present(false);
    if (prompt_step == 3) mock_sd_card_present(true);
    if (prompt_step == 5 && (prompt_script == PROMPT_LAUNCH || prompt_script == PROMPT_COUNTDOWN_REMOVE))
        mock_clock_advance(9000000);
    if (prompt_step == 6 && (prompt_script == PROMPT_LAUNCH || prompt_script == PROMPT_COUNTDOWN_REMOVE))
        mock_clock_advance(850000);
    if (prompt_step == 5 && prompt_script == PROMPT_COUNTDOWN_ABORT) mock_clock_advance(5000000);
    if (prompt_step == 6 && prompt_script == PROMPT_COUNTDOWN_ABORT)
        mock_input_uart_bytes((const uint8_t*)" ", 1);
    if (prompt_step == 5 && prompt_script == PROMPT_COUNTDOWN_REMOVE) mock_sd_card_present(false);
}

/** Escape the wrapped nonreturning diagnostic loop after checking launch state. */
static void observe_sd_launch(diag_session_t* launched) {
    ck_assert_ptr_eq(launched->plan, &diag_plan_sd);
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_uint_ge(prompt_step, 7);
    diagnostic_observed = true;
    longjmp(diagnostic_escape, 1);
}

/** Record video startup before the automatic diagnostic owns the display. */
static void observe_boot_video(void) { ++boot_video_calls; }

/** Stop at the real bring-up loop boundary without running the destructive cases. */
static void observe_bup_launch(diag_session_t* launched) {
    ck_assert_ptr_eq(launched->plan, &diag_plan_bup);
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_uint_eq(boot_video_calls, 1);
    ck_assert_uint_eq(launched->max_cycles, UINT8_MAX);
    diagnostic_observed = true;
    longjmp(diagnostic_escape, 1);
}

/** Observe CLI launch without executing the destructive plan. */
static void observe_cli_bup_launch(diag_session_t* launched) {
    ck_assert_ptr_eq(launched->plan, &diag_plan_bup);
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    observed_max_cycles = launched->max_cycles;
    diagnostic_observed = true;
    longjmp(diagnostic_escape, 1);
}

/** Escape the finite-pass completion halt endpoint. */
static void observe_complete_halt(void) {
    complete_observed = true;
    longjmp(diagnostic_escape, 1);
}

/** Reset shared diagnostic and hardware fixtures without leaking CK_NOFORK state. */
static void setup(void) {
    saved_state = system_state;
    log_init();
    mock_reset();
    ieee_drive_init();
    mock_clock_set(0);
    memset(calls, 0, sizeof(calls));
    next_call = 0;
    inject_failure = false;
    scroll_calls = 0;
    diagnostic_observed = false;
    swap_prepare_result = true;
    swap_prepare_calls = 0;
    reset_observed = false;
    complete_observed = false;
    observed_max_cycles = 0;
    prompt_step = 0;
    boot_video_calls = 0;
    mock_sd_prepare_set_handler(observe_swap_prepare);
    mock_system_reset_set_handler(observe_reset);
    capture = NULL;
    saved_stdout = -1;
}

/** Restore stdout and shared renderer state, even after a failed expectation. */
static void teardown(void) {
    if (saved_stdout >= 0) {
        fflush(stdout);
        dup2(saved_stdout, fileno(stdout));
        close(saved_stdout);
    }
    if (capture != NULL) fclose(capture);
    mock_diag_loop_set_handler(NULL);
    mock_diag_complete_set_handler(NULL);
    mock_spi_read_set_observer(NULL);
    mock_sd_prepare_set_handler(NULL);
    mock_sleep_set_handler(NULL);
    mock_system_reset_set_handler(NULL);
    mock_video_init_set_handler(NULL);
    mock_spi_clear_fault();
    mock_clock_restore();
    system_state = saved_state;
}

/** The boot check preserves normal startup or launches bup without menu input. */
START_TEST(test_bup_boot_routing) {
    mock_video_init_set_handler(observe_boot_video);
    mock_diag_loop_set_handler(observe_bup_launch);
    mock_pia1_fault(_i != 0, 0);
    if (setjmp(diagnostic_escape) == 0) bup_boot_check();
    ck_assert_int_eq(diagnostic_observed, _i != 0);
    ck_assert_uint_eq(boot_video_calls, _i != 0 ? 1 : 0);
    if (_i == 0) {
        ck_assert_int_eq(system_mode(), system_operation_boot);
        ck_assert_int_eq(get_cpu(), CPU_RESET);
    }
}
END_TEST

/** Capture production UART printf output through the existing host stdio adapter. */
static void capture_begin(void) {
    capture = tmpfile();
    ck_assert_ptr_nonnull(capture);
    fflush(stdout);
    saved_stdout = dup(fileno(stdout));
    ck_assert_int_ge(saved_stdout, 0);
    ck_assert_int_ge(dup2(fileno(capture), fileno(stdout)), 0);
}

/** Read captured output without leaving stdout redirected during assertions. */
static void capture_end(char* out, size_t size) {
    fflush(stdout);
    ck_assert_int_ge(dup2(saved_stdout, fileno(stdout)), 0);
    close(saved_stdout);
    saved_stdout = -1;
    rewind(capture);
    const size_t length = fread(out, 1, size - 1, capture);
    out[length] = '\0';
    fclose(capture);
    capture = NULL;
}

/** Feed a complete command string through the cooperative CLI character path. */
static void feed_cli(const char* text) {
    while (*text != '\0') cli_process_char(*text++);
}

/** The new CLI name starts the bring-up plan without changing memory's command. */
START_TEST(test_cli_bup_launch) {
    cli_init();
    mock_diag_loop_set_handler(observe_cli_bup_launch);
    if (setjmp(diagnostic_escape) == 0) feed_cli("test bup\r");
    ck_assert(diagnostic_observed);
}
END_TEST

/** Assert ASCII text at an exact coordinate after canonical VROM conversion. */
static void check_text(unsigned col, unsigned row, const char* text) {
    for (size_t i = 0; text[i] != '\0'; ++i)
        ck_assert_uint_eq(system_state.video_char_buffer[row * DIAG_COLS + col + i],
                          ascii_to_vrom((uint8_t)text[i]));
}

/** Assert reverse-video text in the diagnostic banner. */
static void check_banner(unsigned col, const char* text) {
    for (size_t i = 0; text[i] != '\0'; ++i)
        ck_assert_uint_eq(system_state.video_char_buffer[col + i],
                          ascii_to_vrom((uint8_t)text[i]) | 0x80);
}

/** Check the cycle bar's filled and empty glyphs and their colors. */
static void check_progress(unsigned filled) {
    for (unsigned index = 0; index < 44; ++index) {
        const unsigned offset = 23 * DIAG_COLS + 34 + index;
        ck_assert_uint_eq(system_state.video_char_buffer[offset], index < filled ? 0xa0 : 0x66);
        ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + offset],
                          DISPLAY_ATTR(index < filled ? DISPLAY_COLOR_LIGHT_BLUE : DISPLAY_COLOR_BLUE,
                                    DISPLAY_COLOR_BLACK));
    }
}

/** Nested failure helper exercises unwinding out of more than the case callback. */
static void nested_check(void) {
    diag_check(!inject_failure, "expected $%02X, got $%02X", 0x55u, 0x54u);
}

/** Verify suite context and declaration order before recording this case. */
static void ordered_case(const void* context) {
    const unsigned index = *(const unsigned*)context + (unsigned)(next_call % 2);
    ck_assert_uint_eq(index, next_call % 4);
    ++calls[index];
    ++next_call;
    check_text(3, 19, index % 2 ? "Second" : "First");
    check_text(75, 19, "RUN");
    check_progress((unsigned)((index * 44) / 4));
    if (next_call == 5) {
        check_text(1, 14, "Suite B");
        check_text(3, 16, "Second");
        check_text(76, 16, "OK");
        check_text(33, 17, " Begin Cycle #2 ");
        check_text(1, 18, "Suite A");
        char elapsed[32], heading[80];
        diag_format_elapsed(time_us_64() - session.start_time_us, elapsed, sizeof(elapsed));
        snprintf(heading, sizeof(heading), "Completed: 1 | Time: %s ", elapsed);
        check_banner(DIAG_COLS - (unsigned)strlen(heading), "Completed: 1");
    }
    if (index == 1) nested_check();
}

static const unsigned contexts[] = { 0, 2 };
static const diag_case_t cases[] = {
    { "First", ordered_case }, { "Second", ordered_case },
};
static const diag_suite_t suites[] = {
    DIAG_SUITE("Suite A", cases, &contexts[0]),
    DIAG_SUITE("Suite B", cases, &contexts[1]),
};
static const diag_plan_t plan = {
    .name = "synthetic",
    .description = "Fixture plan",
    .confirm = NULL,
    .suites = suites,
    .suite_count = count_of(suites),
};

/** Two full passes preserve case ordering, cumulative passes, and local RGBI output. */
START_TEST(test_iterations_and_isolated_display) {
    memset(mock_ram + VIDEO_RAM_ADDRESS, 0xa5, DIAG_COLS * DIAG_ROWS);
    uint8_t fpga_font[sizeof(rom_chars_e800)];
    memset(fpga_font, 0x5a, sizeof(fpga_font));
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR, fpga_font, sizeof(fpga_font));
    roms_refresh_char_rom();
    const unsigned accesses = mock_spi_access_count();

    diag_session_init(&session, &plan);
    check_progress(0);
    ck_assert(diag_run_iteration(&session));
    mock_clock_advance(UINT64_C(3661000000));
    ck_assert(diag_run_iteration(&session));
    ck_assert_uint_eq(mock_spi_access_count(), accesses);
    ck_assert_uint_eq(mock_display_task_count(), 0);
    for (size_t i = 0; i < count_of(calls); ++i) {
        ck_assert_uint_eq(calls[i], 2);
    }
    ck_assert_uint_eq(session.iteration, 2);
    ck_assert_int_eq(session.active_status, DIAG_STATUS_PASS);
    check_text(33, 6, " Begin Cycle #1 ");
    check_text(1, 7, "Suite A");
    check_text(3, 8, "First");
    check_text(76, 8, "OK");
    check_text(8, 8, "...");
    check_text(33, 13, " Begin Cycle #2 ");
    check_text(1, 14, "Suite A");
    check_text(3, 19, "Second");
    check_text(76, 19, "OK");
    check_banner(0, " EconoPET Synthetic Diagnostic");
    check_banner(DIAG_COLS - (sizeof("Completed: 2 | Time: 01:01:01 ") - 1), "Completed: 2");
    check_banner(DIAG_COLS - (sizeof("Time: 01:01:01 ") - 1), "Time: 01:01:01 ");
    check_text(2, 21, "Suite:");
    check_text(9, 21, "Suite B");
    check_text(3, 22, "Case:");
    check_text(9, 22, "Second");
    check_text(2, 23, "Cycle: 2");
    check_text(24, 23, "Progress:");
    check_progress(44);
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET],
                      DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_GRAY, DISPLAY_COLOR_BLACK));
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + 19 * DIAG_COLS + 76],
                      DISPLAY_ATTR(DISPLAY_COLOR_GREEN, DISPLAY_COLOR_BLACK));
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + 13 * DIAG_COLS],
                      DISPLAY_ATTR(DISPLAY_COLOR_DARK_MAGENTA, DISPLAY_COLOR_BLACK));
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + 13 * DIAG_COLS + 33],
                      DISPLAY_ATTR(DISPLAY_COLOR_MAGENTA, DISPLAY_COLOR_BLACK));
    ck_assert_uint_eq(system_state.video_char_buffer[13 * DIAG_COLS],
                      system_state.video_char_buffer[20 * DIAG_COLS + 1]);
    ck_assert_uint_eq(system_state.video_char_buffer[13 * DIAG_COLS + 79],
                      system_state.video_char_buffer[20 * DIAG_COLS + 1]);
    ck_assert_uint_eq(system_state.video_char_buffer[20 * DIAG_COLS], 0x70);
    ck_assert_uint_eq(system_state.video_char_buffer[24 * DIAG_COLS + 79], 0x7d);
    for (size_t i = 0; i < DIAG_COLS * DIAG_ROWS; ++i)
        ck_assert_uint_eq(mock_ram[VIDEO_RAM_ADDRESS + i], 0xa5);
    spi_read(ECONOPET_WB_BRAM_BASE_ADDR, sizeof(fpga_font), fpga_font);
    for (size_t i = 0; i < sizeof(fpga_font); ++i) ck_assert_uint_eq(fpga_font[i], 0x5a);
    ck_assert_mem_eq(roms_get_char_rom(video_text_mode_lower), rom_chars_e800 + 0x400, 0x400);

    // Exercise the actual geometry calculation used by Core 1, not just register values.
    dvi_display_geometry_t geometry;
    crtc_calculate_geometry(system_state.pet_crtc_registers, system_state.pet_display_columns,
                            720, 240, 8, 2, &geometry);
    ck_assert_uint_eq(geometry.chars_per_row, DIAG_COLS);
    ck_assert_uint_eq(geometry.rows, DIAG_ROWS);
    ck_assert_uint_eq(geometry.vram_start, 0);
    ck_assert_uint_eq(geometry.invert_mask, 0);
    ck_assert_uint_eq(geometry.vram_mask, VIDEO_PLANE_CAPACITY - 1);
    diag_session_init(&session, &plan);
    check_text(1, 9, "       ");
    check_text(3, 19, "      ");
}
END_TEST

/** A raw Escape aborts immediately without waiting for terminal sequence decoding. */
START_TEST(test_cli_sd_escape_aborts) {
    system_reset_guest();
    cli_init();
    prompt_script = PROMPT_ESCAPE_ABORT;
    mock_sleep_set_handler(drive_prompt);
    if (setjmp(diagnostic_escape) == 0) feed_cli("test sd\r");
    ck_assert(reset_observed);
    ck_assert_uint_eq(prompt_step, 1);
    ck_assert(!diagnostic_observed);
}
END_TEST

/** Observe the elapsed banner updating while the same case is still running. */
static void long_running_case(const void* context) {
    (void)context;
    mock_clock_advance(1000000);
    diag_refresh();
    check_banner(DIAG_COLS - (sizeof("Completed: 0 | Time: 00:00:01 ") - 1), "Completed: 0");
    check_banner(DIAG_COLS - (sizeof("Time: 00:00:01 ") - 1), "Time: 00:00:01 ");
    check_text(3, 19, "Long case");
    check_text(75, 19, "RUN");
    check_progress(0);
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + 19 * DIAG_COLS + 75],
                      DISPLAY_ATTR(DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_BLACK));
}

/** Long cases can yield display updates without returning or touching SPI. */
START_TEST(test_live_elapsed_refresh) {
    const diag_case_t tests_arr[] = { { "Long case", long_running_case } };
    const diag_suite_t suite = DIAG_SUITE("Live", tests_arr, NULL);
    const diag_plan_t live_plan = {
        .name = "live", .description = "Fixture", .suites = &suite, .suite_count = 1
    };
    const unsigned accesses = mock_spi_access_count();
    diag_session_init(&session, &live_plan);
    ck_assert(diag_run_iteration(&session));
    check_progress(44);
    ck_assert_uint_eq(mock_spi_access_count(), accesses);
}
END_TEST

/** Complete one case and refresh before failing the second case. */
static void partial_cycle_case(const void* context) {
    if (context == NULL) return;
    mock_clock_advance(1000000);
    diag_refresh();
    check_progress(22);
    diag_fail("partial case failure");
}

/** Only completed cases contribute to the cycle bar, including on failure. */
START_TEST(test_cycle_progress_on_failure) {
    const diag_case_t first[] = { { "First", partial_cycle_case } };
    const diag_case_t second[] = { { "Second", partial_cycle_case } };
    const diag_suite_t cycle_suites[] = {
        DIAG_SUITE("First suite", first, NULL),
        DIAG_SUITE("Second suite", second, &session),
    };
    const diag_plan_t cycle_plan = {
        .name = "cycle", .description = "Fixture", .suites = cycle_suites, .suite_count = count_of(cycle_suites)
    };
    diag_session_init(&session, &cycle_plan);
    ck_assert(!diag_run_iteration(&session));
    check_text(2, 23, "Cycle: 1");
    check_progress(22);
    check_banner(DIAG_COLS - (sizeof("Completed: 0 | Time: 00:00:01 ") - 1), "Completed: 0");
    check_text(9, 22, "partial case failure");
}
END_TEST

/** Large cycle numbers remain readable without colliding with the progress bar. */
START_TEST(test_large_cycle_number) {
    const diag_case_t test_arr[] = { { "Case", partial_cycle_case } };
    const diag_suite_t suite = DIAG_SUITE("Suite", test_arr, NULL);
    const diag_plan_t cycle_plan = {
        .name = "cycle", .description = "Fixture", .suites = &suite, .suite_count = 1
    };
    diag_session_init(&session, &cycle_plan);
    session.iteration = UINT64_MAX - 1;
    ck_assert(diag_run_iteration(&session));
    check_text(9, 23, "18446744073709551615");
    check_text(30, 23, "Progress:");
    for (unsigned col = 40; col < 78; ++col)
        ck_assert_uint_eq(system_state.video_char_buffer[23 * DIAG_COLS + col], 0xa0);
    check_banner(DIAG_COLS - (sizeof("Completed: 18446744073709551615 | Time: 00:00:00 ") - 1),
                 "Completed: 18446744073709551615");
}
END_TEST

/** Fail the last case after advancing beyond the first display viewport. */
static void scroll_case(const void* context) {
    (void)context;
    ++scroll_calls;
    diag_check(scroll_calls != 64u, "late failure");
}

/** The full supported case capacity scrolls the failing item into view. */
START_TEST(test_scrolling_failure_visible) {
    diag_case_t first_cases[32u], second_cases[32u];
    for (size_t index = 0; index < count_of(first_cases); ++index) {
        first_cases[index] = (diag_case_t) { "Earlier", scroll_case };
        second_cases[index] = (diag_case_t) { "Earlier", scroll_case };
    }
    second_cases[count_of(second_cases) - 1].name = "Last visible";
    const diag_suite_t suites[] = {
        DIAG_SUITE("First large suite", first_cases, NULL),
        DIAG_SUITE("Second large suite", second_cases, NULL),
    };
    const diag_plan_t large_plan = {
        .name = "large", .description = "Fixture", .suites = suites, .suite_count = count_of(suites)
    };
    diag_session_init(&session, &large_plan);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_uint_eq(scroll_calls, count_of(first_cases) + count_of(second_cases));
    check_text(74, 19, "FAIL");
    check_text(3, 19, "Last visible");
    check_text(9, 22, "late failure");
    check_progress(43);
}
END_TEST

/** Rate-limit rapid iterations without hiding the first completion or long runtimes. */
START_TEST(test_uart_summaries_and_elapsed) {
    char elapsed[32], output[1024];
    diag_format_elapsed(0, elapsed, sizeof(elapsed));
    ck_assert_str_eq(elapsed, "00:00:00");
    diag_format_elapsed(UINT64_C(3661000000), elapsed, sizeof(elapsed));
    ck_assert_str_eq(elapsed, "01:01:01");
    diag_format_elapsed(UINT64_C(450000000000), elapsed, sizeof(elapsed));
    ck_assert_str_eq(elapsed, "125:00:00");
    diag_session_init(&session, &plan);
    capture_begin();
    ck_assert(diag_run_iteration(&session));
    mock_clock_advance(100000);
    ck_assert(diag_run_iteration(&session));
    mock_clock_advance(1000000);
    ck_assert(diag_run_iteration(&session));
    capture_end(output, sizeof(output));
    ck_assert_ptr_nonnull(strstr(output, "iter #1 (00:00:00)"));
    ck_assert_ptr_null(strstr(output, "iter #2"));
    ck_assert_ptr_nonnull(strstr(output, "iter #3 (00:00:01)"));
}
END_TEST

/** Failure freezes the case and prevents later suites or repeat iterations from executing. */
START_TEST(test_first_failure_retained) {
    char output[1024];
    diag_session_init(&session, &plan);
    inject_failure = true;
    capture_begin();
    ck_assert(!diag_run_iteration(&session));
    capture_end(output, sizeof(output));
    ck_assert_uint_eq(calls[0], 1);
    ck_assert_uint_eq(calls[1], 1);
    ck_assert_uint_eq(calls[2], 0);
    ck_assert_uint_eq(calls[3], 0);
    ck_assert_int_eq(session.active_status, DIAG_STATUS_FAIL);
    ck_assert_uint_eq(session.active_suite_index, 0);
    ck_assert_uint_eq(session.active_case_index, 1);
    ck_assert_str_eq(session.failure_detail, "expected $55, got $54");
    ck_assert_ptr_nonnull(strstr(output, "DIAG FAIL [synthetic] iter 1 (00:00:00) Suite A / Second"));
    ck_assert_ptr_nonnull(strstr(output, session.failure_detail));
    check_text(74, 19, "FAIL");
    check_text(3, 19, "Second");
    check_text(9, 22, session.failure_detail);
    check_progress(11);
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET],
                      DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_GRAY, DISPLAY_COLOR_BLACK));
    uint8_t screen[DIAG_COLS * DIAG_ROWS];
    memcpy(screen, system_state.video_char_buffer, sizeof(screen));
    mock_clock_advance(1000000);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_uint_eq(next_call, 2);
    ck_assert_mem_eq(screen, system_state.video_char_buffer, sizeof(screen));
}
END_TEST

/** Missing, unknown, and invalid count arguments return to the CLI without destructive takeover. */
START_TEST(test_cli_usage_preserves_ownership) {
    char output[4096];
    const char* const commands[] = {
        "test\r", "test   \r", "test bogus\r", "test memory extra\r",
        "test memory 0\r", "test memory -2\r", "test memory 256\r", "test memory 2 extra\r",
    };
    system_reset_guest();
    set_cpu(CPU_HALT);
    ck_assert(system_suspend());
    cli_init();
    const unsigned accesses = mock_spi_access_count();
    capture_begin();
    for (size_t index = 0; index < count_of(commands); ++index) {
        for (const char* ch = commands[index]; *ch != '\0'; ++ch) cli_process_char(*ch);
        ck_assert_int_eq(system_mode(), system_operation_console);
    }
    capture_end(output, sizeof(output));
    ck_assert_uint_eq(mock_spi_access_count(), accesses);
    ck_assert_ptr_nonnull(strstr(output, "Usage: test <plan> [count]"));
    ck_assert_ptr_nonnull(strstr(output, "Invalid cycle count"));
    ck_assert_ptr_nonnull(strstr(output, "memory"));
    ck_assert_ptr_nonnull(strstr(output, "bup"));
    ck_assert_ptr_nonnull(strstr(output, "drive"));
    ck_assert_ptr_nonnull(strstr(output, "econopet>"));
    system_restore(false);
}
END_TEST

/** CLI cycle counts default to 1, accept 1..255, and convert -1 to 255. */
START_TEST(test_cli_cycle_counts) {
    static const struct {
        const char* cmd;
        uint8_t expected;
    } cases_table[] = {
        {"test bup\r", 1u},
        {"test bup 7\r", 7u},
        {"test bup 255\r", 255u},
        {"test bup -1\r", 255u},
        {"test   bup   7   \r", 7u},
        {"test   bup   \r", 1u},
    };

    // Intercept diag_loop() and verify that CLI parsing populates launched->max_cycles.
    cli_init();
    mock_diag_loop_set_handler(observe_cli_bup_launch);
    if (setjmp(diagnostic_escape) == 0) feed_cli(cases_table[_i].cmd);
    ck_assert(diagnostic_observed);
    ck_assert_uint_eq(observed_max_cycles, cases_table[_i].expected);
}
END_TEST

/** CRLF is not an abort, but a later key resets without resuming the guest. */
START_TEST(test_cli_sd_prompt_cancel) {
    char output[4096];
    system_reset_guest();
    cli_init();
    prompt_script = PROMPT_ABORT;
    mock_sleep_set_handler(drive_prompt);
    capture_begin();
    mock_input_uart_bytes((const uint8_t*)"test sd\r\n", sizeof("test sd\r\n") - 1);
    if (setjmp(diagnostic_escape) == 0) input_uart_task();
    ck_assert_uint_eq(swap_prepare_calls, 1);
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    capture_end(output, sizeof(output));
    ck_assert(reset_observed);
    ck_assert_uint_eq(prompt_step, 1);
    ck_assert(!diagnostic_observed);
    ck_assert_ptr_nonnull(strstr(output, "WARNING: 'test sd' DESTROYS ALL DATA"));
    ck_assert_ptr_nonnull(strstr(output, "Remove the current card"));
    ck_assert_ptr_nonnull(strstr(output, "Aborted. Restarting."));
}
END_TEST

/** Any key during the countdown resets instead of starting destructive work. */
START_TEST(test_cli_sd_countdown_abort) {
    char output[4096];
    system_reset_guest();
    cli_init();
    mock_diag_loop_set_handler(observe_sd_launch);
    prompt_script = PROMPT_COUNTDOWN_ABORT;
    mock_sleep_set_handler(drive_prompt);
    capture_begin();
    if (setjmp(diagnostic_escape) == 0) feed_cli("test sd\r");
    capture_end(output, sizeof(output));
    ck_assert(reset_observed);
    ck_assert(!diagnostic_observed);
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_ptr_nonnull(strstr(output, "10... 9... 8... 7... 6... 5..."));
    ck_assert_ptr_nonnull(strstr(output, "Aborted."));
}
END_TEST

/** The SD plan starts destructive work only after a stable swap and countdown. */
START_TEST(test_cli_sd_countdown_completes) {
    char output[4096];
    system_reset_guest();
    cli_init();
    mock_diag_loop_set_handler(observe_sd_launch);
    prompt_script = PROMPT_LAUNCH;
    mock_sleep_set_handler(drive_prompt);
    capture_begin();
    if (setjmp(diagnostic_escape) == 0) feed_cli("test sd\r");
    capture_end(output, sizeof(output));
    ck_assert(diagnostic_observed);
    ck_assert(!reset_observed);
    ck_assert_ptr_nonnull(strstr(output, "POWER OFF"));
    ck_assert_ptr_nonnull(strstr(output, " 1... 0"));
}
END_TEST

/** Contact bounce and a card initially present cannot start the countdown. */
START_TEST(test_cli_sd_debounces_swap) {
    char output[4096];
    system_reset_guest();
    cli_init();
    mock_diag_loop_set_handler(observe_sd_launch);
    prompt_script = PROMPT_BOUNCE;
    mock_sleep_set_handler(drive_prompt);
    capture_begin();
    if (setjmp(diagnostic_escape) == 0) feed_cli("test sd\r");
    capture_end(output, sizeof(output));
    ck_assert(reset_observed);
    ck_assert(!diagnostic_observed);
    ck_assert_ptr_null(strstr(output, "Starting 'sd'"));
}
END_TEST

/** A socket already empty at the prompt still requires a stable insertion. */
START_TEST(test_cli_sd_initially_absent) {
    char output[4096];
    system_reset_guest();
    mock_sd_card_present(false);
    cli_init();
    prompt_script = PROMPT_INITIAL_ABSENT;
    mock_sleep_set_handler(drive_prompt);
    capture_begin();
    if (setjmp(diagnostic_escape) == 0) feed_cli("test sd\r");
    capture_end(output, sizeof(output));
    ck_assert(reset_observed);
    ck_assert_ptr_nonnull(strstr(output, "Card removed."));
    ck_assert_ptr_nonnull(strstr(output, "Card inserted."));
    ck_assert(!diagnostic_observed);
}
END_TEST

/** Removing the card during countdown does not abort or shorten the countdown. */
START_TEST(test_cli_sd_removal_during_countdown) {
    char output[4096];
    system_reset_guest();
    cli_init();
    mock_diag_loop_set_handler(observe_sd_launch);
    prompt_script = PROMPT_COUNTDOWN_REMOVE;
    mock_sleep_set_handler(drive_prompt);
    capture_begin();
    if (setjmp(diagnostic_escape) == 0) feed_cli("test sd\r");
    capture_end(output, sizeof(output));
    ck_assert(!reset_observed);
    ck_assert(diagnostic_observed);
    ck_assert_ptr_nonnull(strstr(output, " 1... 0"));
}
END_TEST

/** Verify failed preparation retains terminal ownership without starting a test. */
static void check_sd_prepare_failure(void) {
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_uint_eq(swap_prepare_calls, 1);
    ck_assert(!diagnostic_observed);
    ck_assert(!reset_observed);
}

/** A failed flush must halt before removal or diagnostic launch. */
START_TEST(test_cli_sd_prepare_failure) {
    system_reset_guest();
    cli_init();
    swap_prepare_result = false;
    test_expect_fatal_message("Could not safely release the SD card.");
    test_expect_fatal_callback(check_sd_prepare_failure);
    feed_cli("test sd\r");
}
END_TEST

/** A menu-originated abort takes over and resets without returning into the menu. */
START_TEST(test_cli_sd_from_menu) {
    char output[4096];
    system_reset_guest();
    ck_assert(system_suspend());
    cli_init();
    prompt_script = PROMPT_ABORT;
    mock_sleep_set_handler(drive_prompt);
    capture_begin();
    if (setjmp(diagnostic_escape) == 0) feed_cli("test sd\r");
    capture_end(output, sizeof(output));
    ck_assert_uint_eq(swap_prepare_calls, 1);
    ck_assert(reset_observed);
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_ptr_nonnull(strstr(output, "Remove the current card"));
}
END_TEST

/** Both plans run the complete memory test without changing local glyphs. */
START_TEST(test_memory_and_bup_plans_pass) {
    diag_session_init(&session, _i == 0 ? &diag_plan_memory : &diag_plan_bup);
    ck_assert(diag_run_iteration(&session));
    ck_assert_int_eq(session.active_status, DIAG_STATUS_PASS);
    ck_assert_mem_eq(roms_get_char_rom(video_text_mode_lower), rom_chars_e800 + 0x400, 0x400);
    ck_assert_uint_eq(mock_ram[MOCK_RAM_SIZE - 1], 0);
}
END_TEST

/** Bring-up also reports failures in either of its current memory suites. */
START_TEST(test_bup_plan_detects_memory_faults) {
    const uint32_t address = _i == 0 ? 0x12345u : ECONOPET_WB_BRAM_BASE_ADDR + 0x1abu;
    mock_spi_inject_fault(address, _i == 0 ? 0x08 : 0, _i == 0 ? 0 : 0x80);
    diag_session_init(&session, &diag_plan_bup);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, _i == 0 ? "$12345" : "$681AB"));
}
END_TEST

/** Inject real SRAM and BRAM read faults and require the exact failing location. */
START_TEST(test_memory_plan_detects_faults) {
    const uint32_t address = _i == 0 ? 0x12345u : ECONOPET_WB_BRAM_BASE_ADDR + 0x1abu;
    mock_spi_inject_fault(address, _i == 0 ? 0x08 : 0, _i == 0 ? 0 : 0x80);
    diag_session_init(&session, &diag_plan_memory);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_uint_eq(session.active_suite_index, (unsigned)_i);
    ck_assert_uint_eq(session.active_case_index, 1);
    ck_assert_ptr_nonnull(strstr(session.failure_detail, _i == 0 ? "$12345" : "$681AB"));
}
END_TEST

static unsigned custom_begin_cycles;
static uint8_t last_begin_cycle_arg;
static unsigned custom_cleanup_calls;
static unsigned generated_runs;
static unsigned custom_prepare_calls;

/** Simulate configuration loading that changes display state before the runner initializes its viewport. */
static bool record_prepare(char* error_out, size_t error_size) {
    ++custom_prepare_calls;
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    display_init_local(pet_display_columns_40, DISPLAY_ATTR(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_BLACK));
    if (inject_failure) {
        snprintf(error_out, error_size, "fixture setup failed");
        return false;
    }
    return true;
}

/** Record each 1-based begin_cycle invocation ordinal. */
static void record_begin_cycle(uint8_t cycle) {
    ++custom_begin_cycles;
    last_begin_cycle_arg = cycle;
}

/** Count plan->cleanup() invocations at the end of diag_loop(). */
static void record_cleanup(void) {
    ++custom_cleanup_calls;
}

/** Format a synthetic generated case name into caller scratch storage. */
static const char* generated_case_name(const diag_suite_t* suite, size_t index, char* buf, size_t size) {
    (void)suite;
    snprintf(buf, size, "Gen #%zu", index + 1u);
    return buf;
}

/** Execute one synthetic generated case, failing index 1 when inject_failure is set. */
static void generated_run_case(const diag_suite_t* suite, size_t index) {
    (void)suite;
    ++generated_runs;
    diag_check(!inject_failure || index != 1u, "gen fail %zu", index);
}

/** Finite diag_loop runs begin_cycle per iteration, renders generated cases, cleans up, and halts cleanly. */
START_TEST(test_finite_loop_and_generated_callbacks) {
    // Configure a 3-case generated suite with begin_cycle and cleanup hooks.
    custom_begin_cycles = 0;
    last_begin_cycle_arg = 0;
    custom_cleanup_calls = 0;
    generated_runs = 0;
    custom_prepare_calls = 0;
    const diag_suite_t gen_suite = {
        .name = "Generated Suite",
        .case_count = 3,
        .case_name = generated_case_name,
        .run_case = generated_run_case,
    };
    const diag_plan_t gen_plan = {
        .name = "gen",
        .description = "Generated fixture",
        .prepare = record_prepare,
        .begin_cycle = record_begin_cycle,
        .cleanup = record_cleanup,
        .suites = &gen_suite,
        .suite_count = 1,
    };

    // Run 2 cycles through diag_run_plan() and escape at diag_complete_halt().
    char output[1024];
    mock_diag_complete_set_handler(observe_complete_halt);
    capture_begin();
    if (setjmp(diagnostic_escape) == 0) {
        diag_run_plan(&gen_plan, 2);
    }
    capture_end(output, sizeof(output));

    // Verify lifecycle call counts, UART completion output, and the rendered completion banner.
    ck_assert(complete_observed);
    ck_assert_uint_eq(custom_prepare_calls, 1);
    ck_assert_uint_eq(custom_begin_cycles, 2);
    ck_assert_uint_eq(last_begin_cycle_arg, 2);
    ck_assert_uint_eq(generated_runs, 6);
    ck_assert_uint_eq(custom_cleanup_calls, 1);
    ck_assert_ptr_nonnull(strstr(output, "DIAG COMPLETE [gen] 2 cycle(s) passed"));
    check_text(3, 18, "Gen #3");
    check_text(76, 18, "OK");
    check_text(30, 19, " All Cycles Completed ");
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + 19 * DIAG_COLS],
                      DISPLAY_ATTR(DISPLAY_COLOR_DARK_MAGENTA, DISPLAY_COLOR_BLACK));
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + 19 * DIAG_COLS + 30],
                      DISPLAY_ATTR(DISPLAY_COLOR_MAGENTA, DISPLAY_COLOR_BLACK));
    ck_assert_uint_eq(system_state.video_char_buffer[19 * DIAG_COLS],
                      system_state.video_char_buffer[20 * DIAG_COLS + 1]);
    check_banner(DIAG_COLS - (sizeof("Completed: 2 | Time: 00:00:00 ") - 1), "Completed: 2");
}
END_TEST

/** Verify setup errors get their own display attribution and release partial resources before fatal halt. */
static void check_terminal_prepare_failure(void) {
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_uint_eq(custom_prepare_calls, 1);
    ck_assert_uint_eq(custom_cleanup_calls, 1);
    ck_assert_uint_eq(custom_begin_cycles, 0);
    ck_assert_uint_eq(generated_runs, 0);
    check_text(2, 21, "Stage");
    check_text(9, 21, "Preparation");
    check_text(9, 22, "fixture setup failed");
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET],
                      DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_GRAY, DISPLAY_COLOR_BLACK));
}

/** A failed prepare result skips all cycle/case callbacks but still initializes the display and cleans up. */
START_TEST(test_terminal_prepare_failure) {
    custom_prepare_calls = 0;
    custom_cleanup_calls = 0;
    custom_begin_cycles = 0;
    generated_runs = 0;
    inject_failure = true;
    const diag_suite_t gen_suite = {
        .name = "Generated Suite",
        .case_count = 1,
        .case_name = generated_case_name,
        .run_case = generated_run_case,
    };
    const diag_plan_t gen_plan = {
        .name = "gen",
        .description = "Preparation failure fixture",
        .prepare = record_prepare,
        .begin_cycle = record_begin_cycle,
        .cleanup = record_cleanup,
        .suites = &gen_suite,
        .suite_count = 1,
    };
    test_expect_fatal_message("DIAG FAIL [gen] preparation: fixture setup failed");
    test_expect_fatal_callback(check_terminal_prepare_failure);
    diag_run_plan(&gen_plan, 2);
}
END_TEST

#define TEST_SEQLOCK_PHASE 5u
#define TEST_SEQLOCK_SLOT 2u

static unsigned seqlock_reads;

/** Publish a consistent snapshot on the third burst after odd or changing even samples. */
static void advance_seqlock_writer(uint32_t addr) {
    // Ignore unrelated SPI reads and commit an even SEQ=2 update on the third mailbox burst.
    if (addr != TARGET_DIAG_MAILBOX_ADDR) {
        return;
    }
    ++seqlock_reads;
    if (seqlock_reads == 2u && (mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] & 1u) == 0u) {
        mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] = 4u;
    }
    if (seqlock_reads == 3u) {
        mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = TEST_SEQLOCK_PHASE;
        mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = TEST_SEQLOCK_SLOT;
        mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = 1u;
        mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_PASSED;
        mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] = 2u;
    }
}

/** Poll a target case inside an armed diagnostic session. */
static void run_target_case(const diag_suite_t* suite, size_t index) {
    const diag_target_runner_t* const runner = suite->context;
    diag_target_await_case(runner, index);
}

/** Mailbox acquisition retries odd or differing snapshots and returns the verified data directly. */
START_TEST(test_mailbox_seqlock_retries_until_even_match) {
    // Seed uncommitted coordinates with either an odd SEQ or an even but changing snapshot.
    seqlock_reads = 0;
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0, TARGET_DIAG_MAILBOX_SIZE);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = 0u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = 0xeeu;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = 0xeeu;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] = _i == 0 ? 1u : 2u;

    // Observe the exact read returned by acquisition, without unrelated session or CPU setup.
    diag_target_snapshot_t snap = {0};
    mock_spi_read_set_observer(advance_seqlock_writer);
    ck_assert(diag_target_read_mailbox(&snap));
    mock_spi_read_set_observer(NULL);

    ck_assert_uint_eq(seqlock_reads, 4u);
    ck_assert_uint_eq(snap.seq, 2u);
    ck_assert_uint_eq(snap.completed, 1u);
    ck_assert_uint_eq(snap.phase, TEST_SEQLOCK_PHASE);
    ck_assert_uint_eq(snap.slot, TEST_SEQLOCK_SLOT);
}
END_TEST

/** Keep every even mailbox burst different so acquisition must exhaust its retry budget. */
static void change_even_mailbox(uint32_t addr) {
    if (addr == TARGET_DIAG_MAILBOX_ADDR) {
        ++seqlock_reads;
        ck_assert_uint_lt(seqlock_reads, 100u);
        mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] += 2u;
    }
}

/** Neither persistent odd SEQ nor unmatched even snapshots may overwrite the caller's output. */
START_TEST(test_mailbox_retry_exhaustion_preserves_output) {
    // Publish apparently successful counters but never allow a consistent committed snapshot.
    seqlock_reads = 0;
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0, TARGET_DIAG_MAILBOX_SIZE);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_PASSED;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = 1u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] = _i == 0 ? 1u : 2u;
    diag_target_snapshot_t snap = {
        .state = TARGET_DIAG_STATE_RUNNING,
        .completed = 0u,
        .phase = TARGET_DIAG_PHASE_NONE,
    };

    // Exhaust retries directly, using a changing even writer for the alternate scenario.
    if (_i != 0) mock_spi_read_set_observer(change_even_mailbox);
    ck_assert(!diag_target_read_mailbox(&snap));
    mock_spi_read_set_observer(NULL);
    ck_assert_uint_eq(snap.state, TARGET_DIAG_STATE_RUNNING);
    ck_assert_uint_eq(snap.completed, 0u);
    ck_assert_uint_eq(snap.phase, TARGET_DIAG_PHASE_NONE);
}
END_TEST

/** Advance the polling clock while publishing only unmatched even snapshots. */
static void stall_mailbox_acquisition(uint32_t addr) {
    change_even_mailbox(addr);
    if (addr == TARGET_DIAG_MAILBOX_ADDR) mock_clock_advance(1000u);
}

/** Polling times out instead of accepting a PASSED state from an unverified final sample. */
START_TEST(test_target_poll_rejects_unverified_completion) {
    // Seed valid magic and successful counters so an unverified snapshot would falsely pass.
    diag_target_begin_cycle(1u);
    seqlock_reads = 0;
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0, TARGET_DIAG_MAILBOX_SIZE);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_PASSED;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = 1u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] = 2u;
    const diag_target_runner_t runner = {
        .total_cases = 1u,
        .case_timeout_us = 1000u,
    };
    const diag_suite_t target_suite = {
        .name = "Target",
        .case_count = 1u,
        .context = &runner,
        .case_name = generated_case_name,
        .run_case = run_target_case,
    };
    const diag_plan_t target_plan = {
        .name = "target",
        .description = "Unverified completion fixture",
        .suites = &target_suite,
        .suite_count = 1u,
    };

    // Poll through the real runner and require a timeout without trusting any sampled fields.
    diag_session_init(&session, &target_plan);
    mock_spi_read_set_observer(stall_mailbox_acquisition);
    ck_assert(!diag_run_iteration(&session));
    mock_spi_read_set_observer(NULL);
    ck_assert_str_eq(session.failure_detail, "target timeout waiting for case 1 (no consistent mailbox snapshot)");
}
END_TEST

static unsigned regression_polls;

/** Model a faulty target restarting its completed counter without completing another check. */
static void regress_target_progress(void) {
    ++regression_polls;
    ck_assert_msg(regression_polls < 20u, "regressing target renewed its deadline indefinitely");
    mock_clock_advance(100u);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = regression_polls & 1u;
}

/** A counter regression must not let a finite run poll indefinitely. */
START_TEST(test_target_poll_rejects_regressing_progress) {
    diag_target_begin_cycle(1u);
    regression_polls = 0;
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0, TARGET_DIAG_MAILBOX_SIZE);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_RUNNING;
    const diag_target_runner_t runner = {
        .total_cases = 3u,
        .case_timeout_us = 1000u,
        .service = regress_target_progress,
    };
    const diag_suite_t target_suite = {
        .name = "Target",
        .case_count = 2u,
        .context = &runner,
        .case_name = generated_case_name,
        .run_case = run_target_case,
    };
    const diag_plan_t target_plan = {
        .name = "target",
        .description = "Regressing progress fixture",
        .suites = &target_suite,
        .suite_count = 1u,
    };
    diag_session_init(&session, &target_plan);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_str_eq(session.failure_detail, "target check count regressed from 1 to 0");
}
END_TEST

static bool target_restart_between_cases;

/** Publish progress, optionally restarting between await calls before publishing final success. */
static void restart_between_target_cases(void) {
    ++regression_polls;
    ck_assert_uint_le(regression_polls, 3u);
    mock_clock_advance(100u);
    const uint8_t completed = regression_polls == 1u ? 1u :
                              regression_polls == 2u && target_restart_between_cases ? 0u : 2u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = completed;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] =
        completed == 2u ? TARGET_DIAG_STATE_PASSED : TARGET_DIAG_STATE_RUNNING;
}

/** Arm a fresh target cycle and restart the fixture's target progress script. */
static void begin_target_progress_cycle(uint8_t cycle) {
    regression_polls = 0u;
    diag_target_begin_cycle(cycle);
}

/** Progress history spans case awaits, rather than accepting a restarted target as a new baseline. */
START_TEST(test_target_poll_retains_progress_between_cases) {
    target_restart_between_cases = _i == 0;
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0, TARGET_DIAG_MAILBOX_SIZE);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    const diag_target_runner_t runner = {
        .total_cases = 2u,
        .case_timeout_us = 1000u,
        .service = restart_between_target_cases,
    };
    const diag_suite_t target_suite = {
        .name = "Target",
        .case_count = 2u,
        .context = &runner,
        .case_name = generated_case_name,
        .run_case = run_target_case,
    };
    const diag_plan_t target_plan = {
        .name = "target",
        .description = "Cross-case restart fixture",
        .begin_cycle = begin_target_progress_cycle,
        .suites = &target_suite,
        .suite_count = 1u,
    };
    diag_session_init(&session, &target_plan);
    if (target_restart_between_cases) {
        ck_assert(!diag_run_iteration(&session));
        ck_assert_str_eq(session.failure_detail, "target check count regressed from 1 to 0");
        ck_assert_uint_eq(session.active_case_index, 1u);
    } else {
        ck_assert(diag_run_iteration(&session));
        ck_assert(diag_run_iteration(&session));
        ck_assert_uint_eq(session.iteration, 2u);
    }
}
END_TEST

static uint16_t target_catalog_ordinal;
static unsigned target_failure_formats;

/** Validate fixed catalog coordinates and record which target check the host validates. */
static bool verify_target_catalog(const diag_target_snapshot_t* snap, uint16_t ordinal,
                                  char* out, size_t out_size) {
    target_catalog_ordinal = ordinal;
    if (snap->phase != TEST_SEQLOCK_PHASE || snap->slot != TEST_SEQLOCK_SLOT) {
        snprintf(out, out_size, "target catalog drift");
        return false;
    }
    return true;
}

/** Exercise the plan-specific failure formatter using independently seeded mismatch bytes. */
static void format_target_fixture_failure(const diag_target_snapshot_t* snap, char* out, size_t out_size) {
    ++target_failure_formats;
    ck_assert_uint_eq(snap->expected, 0x55u);
    ck_assert_uint_eq(snap->actual, 0x54u);
    snprintf(out, out_size, "target fixture mismatch");
}

/** Attribute failure to the uncompleted case and reject drift or inconsistent terminal counts. */
START_TEST(test_target_failure_attribution_and_catalog_validation) {
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0, TARGET_DIAG_MAILBOX_SIZE);
    diag_target_begin_cycle(1u);
    target_catalog_ordinal = UINT16_MAX;
    target_failure_formats = 0u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] =
        _i == 2 ? TARGET_DIAG_STATE_PASSED :
        _i == 1 || _i == 4 ? TARGET_DIAG_STATE_RUNNING : TARGET_DIAG_STATE_FAILED;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = _i == 3 ? 0u : _i == 4 ? 3u : 1u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = TEST_SEQLOCK_PHASE;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = _i == 1 ? TEST_SEQLOCK_SLOT + 1u : TEST_SEQLOCK_SLOT;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_DETAIL_ID)] = 7u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_EXPECTED)] = 0x55u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_ACTUAL)] = 0x54u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_OFFSET_LO)] = 2u;
    const diag_target_runner_t runner = {
        .total_cases = 2u,
        .case_timeout_us = 1000u,
        .verify_snapshot = verify_target_catalog,
        .format_failure = _i == 3 ? NULL : format_target_fixture_failure,
    };
    const diag_suite_t target_suite = {
        .name = "Target",
        .case_count = 2u,
        .context = &runner,
        .case_name = generated_case_name,
        .run_case = run_target_case,
    };
    const diag_plan_t target_plan = {
        .name = "target",
        .description = "Failure attribution and catalog fixture",
        .suites = &target_suite,
        .suite_count = 1u,
    };
    const char* const failures[] = {
        "target fixture mismatch",
        "target catalog drift",
        "target check count mismatch (expected 2, got 1)",
        "detail 7 at +2: expected $55, got $54",
        "target check count mismatch (expected 2, got 3)",
    };
    diag_session_init(&session, &target_plan);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_str_eq(session.failure_detail, failures[_i]);
    ck_assert_uint_eq(session.active_case_index, _i == 0 ? 1u : 0u);
    ck_assert_uint_eq(target_failure_formats, _i == 0 ? 1u : 0u);
    ck_assert_uint_eq(target_catalog_ordinal, _i == 0 ? 1u : _i == 2 || _i == 4 ? UINT16_MAX : 0u);
}
END_TEST

/** Shared drive catalog resolves all 468 cases and polls the $7FF0 mailbox cleanly. */
START_TEST(test_drive_catalog_and_target_mailbox) {
    // Walk every suite and case in diag_plan_drive, verifying valid coordinates and bounded labels.
    size_t total = 0;
    char name[DIAG_CASE_NAME_MAX];
    for (size_t s = 0; s < diag_plan_drive.suite_count; ++s) {
        const diag_suite_t* const suite = &diag_plan_drive.suites[s];
        for (size_t c = 0; c < suite->case_count; ++c) {
            diag_drive_case_t desc;
            diag_drive_resolve_case(s, c, &desc);
            ck_assert_uint_lt(desc.phase, DT_PHASE_COUNT);
            ck_assert_uint_lt(desc.slot, DT_SLOT_COUNT);
            const char* const formatted = suite->case_name(suite, c, name, sizeof(name));
            ck_assert_ptr_nonnull(formatted);
            ck_assert_uint_gt(strlen(formatted), 0);
            ck_assert_uint_lt(strlen(formatted), DIAG_COLS - 10u);
            ++total;
        }
    }
    ck_assert_uint_eq(total, DT_TOTAL_CASES);

    // Spot-check representative generated case coordinates across suites.
    diag_drive_case_t desc;
    diag_drive_resolve_case(1, 34, &desc); // Slot 2 (U9/D0), step 0 (Empty Directory)
    ck_assert_uint_eq(desc.phase, DT_PHASE_EMPTY_DIR);
    ck_assert_uint_eq(desc.device, 9);
    ck_assert_uint_eq(desc.drive, 0);

    diag_drive_resolve_case(7, DT_SUITE_MAINT_COUNT - 1u, &desc); // Final D82 zone retention
    ck_assert_uint_eq(desc.phase, DT_PHASE_GEOM_RET);
    ck_assert_uint_eq(desc.device, 10);
    ck_assert_uint_eq(desc.track, 154);
    ck_assert_uint_eq(desc.sector, 22);

    diag_drive_case_t final_desc;
    diag_drive_resolve_case(8, diag_plan_drive.suites[8].case_count - 1u, &final_desc);
    diag_drive_case_t fail_desc;
    diag_drive_resolve_case(1, 34, &fail_desc);

    // Exercise a passing 468-case pass through the $7FF0 mailbox.
    diag_target_begin_cycle(1u);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_PASSED;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = (uint8_t)(DT_TOTAL_CASES & 0xffu);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = (uint8_t)(DT_TOTAL_CASES >> 8);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = final_desc.phase;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = final_desc.slot;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] = 0u;

    const diag_plan_t mailbox_plan = {
        .name = "drive",
        .description = diag_plan_drive.description,
        .suites = diag_plan_drive.suites,
        .suite_count = diag_plan_drive.suite_count,
    };
    diag_session_init(&session, &mailbox_plan);
    ck_assert(diag_run_iteration(&session));
    ck_assert_int_eq(session.active_status, DIAG_STATUS_PASS);

    // Simulate Test #43 (ordinal 42: Single-Drive Smoke index 34 = Empty Directory U9/D0) failing.
    diag_target_begin_cycle(1u);
    diag_session_init(&session, &mailbox_plan);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = 42u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = 0u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_FAILED;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = fail_desc.phase;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = fail_desc.slot;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_DETAIL_ID)] = DT_ERR_STATUS_BYTE;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_EXPECTED)] = 0x31u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_ACTUAL)] = 0x30u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_AUX0)] = 0u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_AUX1)] = 0u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_OFFSET_LO)] = 13u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_OFFSET_HI)] = 0u;

    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_summary, "Empty Directory (U9/D0 8050)"));
    ck_assert_str_eq(session.failure_detail, "status byte at +13: expected $31, got $30");

    // Detect mailbox (phase, slot) drift and surplus completion counts.
    diag_target_begin_cycle(1u);
    diag_session_init(&session, &mailbox_plan);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = 42u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = 0u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_FAILED;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = fail_desc.phase;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = (uint8_t)(fail_desc.slot ^ 1u);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "mailbox drift at check #43"));

    diag_target_begin_cycle(1u);
    diag_session_init(&session, &mailbox_plan);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_PASSED;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = (uint8_t)((DT_TOTAL_CASES + 1u) & 0xffu);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = (uint8_t)((DT_TOTAL_CASES + 1u) >> 8);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = final_desc.phase;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = final_desc.slot;
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "target check count mismatch (expected 468, got 469)"));
}
END_TEST

static unsigned slow_duplicate_reads;

/** Cross-copy source errors survive validation, but success and unrelated slots/phases remain strict. */
START_TEST(test_drive_cross_copy_source_failure) {
    const unsigned scenario = (unsigned)_i % 4u;
    const unsigned source = (unsigned)_i / 4u;
    const uint16_t copy_ordinal = DT_SUITE_FORMAT_COUNT + DT_SUITE_SMOKE_COUNT + DT_SUITE_ISOLATION_COUNT + source;
    diag_target_begin_cycle(1u);
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0, TARGET_DIAG_MAILBOX_SIZE);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] =
        scenario == 1 ? TARGET_DIAG_STATE_RUNNING : TARGET_DIAG_STATE_FAILED;
    const uint16_t completed = copy_ordinal + (scenario == 1 ? 1u : 0u);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = (uint8_t)completed;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = (uint8_t)(completed >> 8);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = scenario == 3 ? DT_PHASE_CROSS_CONCAT : DT_PHASE_CROSS_COPY;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = scenario == 2 ? 2u : (uint8_t)source;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_DETAIL_ID)] = DT_ERR_COPY_SOURCE;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_EXPECTED)] = 0x55u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_ACTUAL)] = 0x54u;

    const diag_plan_t copy_plan = {
        .name = "drive",
        .suites = &diag_plan_drive.suites[3],
        .suite_count = 1,
    };
    diag_session_init(&session, &copy_plan);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(
        strstr(session.failure_detail, scenario == 0 ? dt_detail_hdmi_names[DT_ERR_COPY_SOURCE] : "mailbox drift"));
}
END_TEST

/** Independently specified suite boundaries from the target's run_pass() execution order. */
static const struct {
    uint16_t ordinal;
    uint8_t phase;
    uint8_t slot;
} suite_first_cases[] = {
    {0, DT_PHASE_FULL_FORMAT, 0},  {8, DT_PHASE_EMPTY_DIR, 0},      {144, DT_PHASE_UNIT_ISO, 0},
    {164, DT_PHASE_CROSS_COPY, 1}, {208, DT_PHASE_MULTI_SEQ, 0},    {216, DT_PHASE_REL_2B, 0},
    {273, DT_PHASE_RAW_BLOCK, 0},  {281, DT_PHASE_SEQ_BOUNDARY, 0}, {444, DT_PHASE_DUP_SEED, 0},
};

static size_t boundary_suite_index;
static unsigned boundary_reads;

/** Hold the target at a suite boundary for one poll, then complete its first check. */
static void advance_suite_boundary(uint32_t addr) {
    if (addr != TARGET_DIAG_MAILBOX_ADDR) {
        return;
    }
    ++boundary_reads;
    ck_assert_uint_le(boundary_reads, 4u);
    if (boundary_reads <= 2u) {
        return;
    }
    const uint16_t completed = suite_first_cases[boundary_suite_index].ordinal + 1u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = (uint8_t)completed;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = (uint8_t)(completed >> 8);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = suite_first_cases[boundary_suite_index].phase;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = suite_first_cases[boundary_suite_index].slot;
}

/** Derived offsets wait for the exact target check and decode every first/preceding-last boundary. */
START_TEST(test_drive_suite_boundaries) {
    boundary_suite_index = (size_t)_i;
    boundary_reads = 0;
    diag_target_begin_cycle(1u);
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0, TARGET_DIAG_MAILBOX_SIZE);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_RUNNING;
    const uint16_t completed = suite_first_cases[boundary_suite_index].ordinal;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = (uint8_t)completed;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = (uint8_t)(completed >> 8);
    // Last case coordinates are independent of the decoder and suite counts under test.
    static const uint8_t preceding_phases[] = {
        TARGET_DIAG_PHASE_NONE, DT_PHASE_FULL_FORMAT,   DT_PHASE_USR_STREAM, DT_PHASE_DRIVE_ISO, DT_PHASE_CONCAT_PEER,
        DT_PHASE_MULTI_SEQ,     DT_PHASE_REL_SIDE_NEXT, DT_PHASE_RAW_BLOCK,  DT_PHASE_GEOM_RET,
    };
    static const uint8_t preceding_slots[] = {TARGET_DIAG_SLOT_NONE, 7, 7, 7, 7, 7, 4, 7, 4};
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = preceding_phases[boundary_suite_index];
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = preceding_slots[boundary_suite_index];
    diag_suite_t boundary_suite = diag_plan_drive.suites[boundary_suite_index];
    boundary_suite.case_count = 1;
    const diag_plan_t boundary_plan = {
        .name = "drive",
        .suites = &boundary_suite,
        .suite_count = 1,
    };
    diag_session_init(&session, &boundary_plan);
    mock_spi_read_set_observer(advance_suite_boundary);
    ck_assert(diag_run_iteration(&session));
    mock_spi_read_set_observer(NULL);
    ck_assert_uint_eq(boundary_reads, 4u);
}
END_TEST

/** Simulate a 60-second duplicate step completing well past 30s, then stalling past 240s. */
static void advance_slow_d82_duplicate(uint32_t addr) {
    if (addr != TARGET_DIAG_MAILBOX_ADDR) {
        return;
    }
    ++slow_duplicate_reads;

    // Advance 20s per 2-burst mailbox poll, completing the first duplicate case at 60s.
    if ((slow_duplicate_reads % 2u) == 0u) {
        mock_clock_advance(UINT64_C(20000000));
        if (slow_duplicate_reads == 6u) {
            const uint16_t dup_first = (uint16_t)(DT_TOTAL_CASES - DT_SUITE_DUPLICATE_COUNT + 1u);
            diag_drive_case_t dup0_desc;
            diag_drive_resolve_case(8, 0, &dup0_desc);
            mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = dup0_desc.phase;
            mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = dup0_desc.slot;
            mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = (uint8_t)(dup_first & 0xffu);
            mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = (uint8_t)(dup_first >> 8);
        }
    }
}

/** Long-running 8250 D82 duplicate cases (>30s) succeed within the 240s backstop, while >240s stalls fail. */
START_TEST(test_drive_d82_duplicate_timeout_budget) {
    // Seed the mailbox at the end of suite 7 so suite 8 begins waiting on the first duplicate step.
    const uint16_t dup_base = (uint16_t)(DT_TOTAL_CASES - DT_SUITE_DUPLICATE_COUNT);
    diag_drive_case_t prev_desc;
    diag_drive_resolve_case(7, DT_SUITE_MAINT_COUNT - 1u, &prev_desc);
    diag_target_begin_cycle(1u);
    slow_duplicate_reads = 0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)] = TARGET_DIAG_MAGIC_0;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)] = TARGET_DIAG_MAGIC_1;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_RUNNING;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = (uint8_t)(dup_base & 0xffu);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = (uint8_t)(dup_base >> 8);
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = prev_desc.phase;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = prev_desc.slot;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)] = 0u;

    // Confirm case 0 passes after 60s and case 1 ("Duplicate Disk") times out only after 240s.
    mock_spi_read_set_observer(advance_slow_d82_duplicate);
    const diag_suite_t* const dup_suite = &diag_plan_drive.suites[8];
    const diag_plan_t dup_only_plan = {
        .name = "drive",
        .description = diag_plan_drive.description,
        .suites = dup_suite,
        .suite_count = 1,
    };
    diag_session_init(&session, &dup_only_plan);
    ck_assert(!diag_run_iteration(&session));
    mock_spi_read_set_observer(NULL);
    ck_assert_uint_gt(slow_duplicate_reads, 6u);
    ck_assert_ptr_nonnull(strstr(session.failure_summary, "Duplicate Disk"));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "target timeout waiting for case"));
}
END_TEST

/** Verify the failure screen and cleanup hook run before the terminal fatal boundary. */
static void check_terminal_failure(void) {
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_uint_eq(custom_cleanup_calls, 1);
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET],
                      DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_GRAY, DISPLAY_COLOR_BLACK));
    check_text(74, 19, "FAIL");
    check_text(9, 22, "expected $55, got $54");
}

/** The unwrapped loop runs cleanup before reaching the display-independent fatal endpoint. */
START_TEST(test_terminal_loop_halts) {
    custom_cleanup_calls = 0;
    inject_failure = true;
    const diag_plan_t cleanup_plan = {
        .name = "synthetic",
        .description = "Fixture plan",
        .cleanup = record_cleanup,
        .suites = suites,
        .suite_count = count_of(suites),
    };
    test_expect_fatal_message("DIAG FAIL [synthetic] iter 1 (00:00:00) Suite A / Second: expected $55, got $54");
    test_expect_fatal_callback(check_terminal_failure);
    diag_run_plan(&cleanup_plan, 1);
}
END_TEST

/** Handing a failed session to the terminal loop preserves its failure even when its budget is exhausted. */
START_TEST(test_terminal_loop_preserves_prior_failure) {
    custom_cleanup_calls = 0;
    inject_failure = true;
    const diag_plan_t cleanup_plan = {
        .name = "synthetic",
        .description = "Fixture plan",
        .cleanup = record_cleanup,
        .suites = suites,
        .suite_count = count_of(suites),
    };
    system_takeover();
    diag_session_init(&session, &cleanup_plan);
    session.max_cycles = _i == 0 ? 1u : 2u;
    ck_assert(!diag_run_iteration(&session));
    test_expect_fatal_message("DIAG FAIL [synthetic] iter 1");
    test_expect_fatal_callback(check_terminal_failure);
    mock_diag_complete_set_handler(observe_complete_halt);
    if (setjmp(diagnostic_escape) == 0) diag_loop(&session);
    ck_abort_msg("failed session reached the successful completion halt");
}
END_TEST

/** Reject empty suites before executing callbacks. */
START_TEST(test_invalid_plan_structure) {
    const diag_suite_t invalid_suite = { "Empty", NULL, 0, NULL, NULL, NULL };
    const diag_plan_t invalid_plan = { .name = "invalid", .description = "Fixture", .suites = &invalid_suite, .suite_count = 1 };
    test_expect_fatal_message("invalid diagnostic suite");
    diag_session_init(&session, &invalid_plan);
}
END_TEST

/** Register bounded framework and actual March algorithm tests with the shared runner. */
Suite* diag_suite(void) {
    Suite* const suite = suite_create("diag");
    TCase* const tests = tcase_create("framework");
    tcase_add_checked_fixture(tests, setup, teardown);
    tcase_add_test(tests, test_iterations_and_isolated_display);
    tcase_add_test(tests, test_live_elapsed_refresh);
    tcase_add_test(tests, test_cycle_progress_on_failure);
    tcase_add_test(tests, test_large_cycle_number);
    tcase_add_test(tests, test_scrolling_failure_visible);
    tcase_add_test(tests, test_uart_summaries_and_elapsed);
    tcase_add_test(tests, test_first_failure_retained);
    tcase_add_loop_test(tests, test_bup_boot_routing, 0, 2);
    tcase_add_test(tests, test_cli_usage_preserves_ownership);
    tcase_add_test(tests, test_cli_bup_launch);
    tcase_add_loop_test(tests, test_cli_cycle_counts, 0, 6);
    tcase_add_test(tests, test_finite_loop_and_generated_callbacks);
    tcase_add_loop_test(tests, test_mailbox_seqlock_retries_until_even_match, 0, 2);
    tcase_add_loop_test(tests, test_mailbox_retry_exhaustion_preserves_output, 0, 2);
    tcase_add_test(tests, test_target_poll_rejects_unverified_completion);
    tcase_add_test(tests, test_drive_catalog_and_target_mailbox);
    tcase_add_loop_test(tests, test_drive_cross_copy_source_failure, 0, 8);
    tcase_add_loop_test(tests, test_drive_suite_boundaries, 0, count_of(suite_first_cases));
    tcase_add_test(tests, test_drive_d82_duplicate_timeout_budget);
    tcase_add_test(tests, test_cli_sd_prompt_cancel);
    tcase_add_test(tests, test_cli_sd_escape_aborts);
    tcase_add_test(tests, test_cli_sd_countdown_abort);
    tcase_add_test(tests, test_cli_sd_countdown_completes);
    tcase_add_test(tests, test_cli_sd_debounces_swap);
    tcase_add_test(tests, test_cli_sd_initially_absent);
    tcase_add_test(tests, test_cli_sd_removal_during_countdown);
    tcase_add_test(tests, test_cli_sd_from_menu);
    tcase_add_loop_test(tests, test_memory_and_bup_plans_pass, 0, 2);
    tcase_add_loop_test(tests, test_memory_plan_detects_faults, 0, 2);
    tcase_add_loop_test(tests, test_bup_plan_detects_memory_faults, 0, 2);
    tcase_add_test(tests, test_target_poll_rejects_regressing_progress);
    tcase_add_loop_test(tests, test_target_poll_retains_progress_between_cases, 0, 2);
    tcase_add_loop_test(tests, test_target_failure_attribution_and_catalog_validation, 0, 5);
    suite_add_tcase(suite, tests);
    return suite;
}

/** Register aborting tests with the process-isolated runner. */
Suite* diag_fatal_suite(void) {
    Suite* const suite = suite_create("diag-fatal");
    TCase* const tests = tcase_create("terminal");
    tcase_add_checked_fixture(tests, setup, teardown);
    tcase_add_test_raise_signal(tests, test_terminal_loop_halts, SIGABRT);
    tcase_add_test_raise_signal(tests, test_terminal_prepare_failure, SIGABRT);
    tcase_add_loop_test_raise_signal(tests, test_terminal_loop_preserves_prior_failure, SIGABRT, 0, 2);
    tcase_add_test_raise_signal(tests, test_cli_sd_prepare_failure, SIGABRT);
    tcase_add_test_raise_signal(tests, test_invalid_plan_structure, SIGABRT);
    suite_add_tcase(suite, tests);
    return suite;
}
