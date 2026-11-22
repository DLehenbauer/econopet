// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag_test.h"

#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

#include "diag/diag.h"
#include "diag/log/log.h"
#include "diag/mem.h"
#include "diag/sd.h"
#include "display/char_encoding.h"
#include "display/display.h"
#include "display/dvi/crtc.h"
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
static unsigned prompt_step;
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
    prompt_step = 0;
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
    mock_sd_prepare_set_handler(NULL);
    mock_sleep_set_handler(NULL);
    mock_system_reset_set_handler(NULL);
    mock_spi_clear_fault();
    mock_clock_restore();
    system_state = saved_state;
}

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
    { "Suite A", cases, count_of(cases), &contexts[0] },
    { "Suite B", cases, count_of(cases), &contexts[1] },
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
    const diag_case_t test = { "Long case", long_running_case };
    const diag_suite_t suite = { "Live", &test, 1, NULL };
    const diag_plan_t live_plan = { "live", "Fixture", NULL, NULL, &suite, 1 };
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
    const diag_case_t first = { "First", partial_cycle_case };
    const diag_case_t second = { "Second", partial_cycle_case };
    const diag_suite_t cycle_suites[] = {
        { "First suite", &first, 1, NULL },
        { "Second suite", &second, 1, &session },
    };
    const diag_plan_t cycle_plan = {
        "cycle", "Fixture", NULL, NULL, cycle_suites, count_of(cycle_suites)
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
    const diag_case_t test = { "Case", partial_cycle_case };
    const diag_suite_t suite = { "Suite", &test, 1, NULL };
    const diag_plan_t cycle_plan = { "cycle", "Fixture", NULL, NULL, &suite, 1 };
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
        { "First large suite", first_cases, count_of(first_cases), NULL },
        { "Second large suite", second_cases, count_of(second_cases), NULL },
    };
    const diag_plan_t large_plan = {
        "large", "Fixture", NULL, NULL, suites, count_of(suites)
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

/** Missing, unknown, and extra plan arguments return to the CLI without destructive takeover. */
START_TEST(test_cli_usage_preserves_ownership) {
    char output[4096];
    const char* const commands[] = { "test\r", "test   \r", "test bogus\r", "test memory extra\r" };
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
    ck_assert_ptr_nonnull(strstr(output, "Usage: test <plan>"));
    ck_assert_ptr_nonnull(strstr(output, "memory"));
    ck_assert_ptr_nonnull(strstr(output, "econopet>"));
    system_restore(false);
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

/** The real memory plan covers all 128 KiB plus BRAM without changing local glyphs. */
START_TEST(test_memory_plan_passes) {
    diag_session_init(&session, &diag_plan_memory);
    ck_assert(diag_run_iteration(&session));
    ck_assert_int_eq(session.active_status, DIAG_STATUS_PASS);
    ck_assert_mem_eq(roms_get_char_rom(video_text_mode_lower), rom_chars_e800 + 0x400, 0x400);
    ck_assert_uint_eq(mock_ram[MOCK_RAM_SIZE - 1], 0);
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

/** Verify the failure screen has not been replaced at the terminal fatal boundary. */
static void check_terminal_failure(void) {
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET],
                      DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_GRAY, DISPLAY_COLOR_BLACK));
    check_text(74, 19, "FAIL");
    check_text(9, 22, "expected $55, got $54");
}

/** The unwrapped infinite loop must reach the existing display-independent fatal endpoint. */
START_TEST(test_terminal_loop_halts) {
    inject_failure = true;
    test_expect_fatal_message("DIAG FAIL [synthetic] iter 1 (00:00:00) Suite A / Second: expected $55, got $54");
    test_expect_fatal_callback(check_terminal_failure);
    diag_run_plan(&plan);
}
END_TEST

/** Reject empty suites before executing callbacks. */
START_TEST(test_invalid_plan_structure) {
    const diag_suite_t invalid_suite = { "Empty", NULL, 0, NULL };
    const diag_plan_t invalid_plan = { "invalid", "Fixture", NULL, NULL, &invalid_suite, 1 };
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
    tcase_add_test(tests, test_cli_usage_preserves_ownership);
    tcase_add_test(tests, test_cli_sd_prompt_cancel);
    tcase_add_test(tests, test_cli_sd_escape_aborts);
    tcase_add_test(tests, test_cli_sd_countdown_abort);
    tcase_add_test(tests, test_cli_sd_countdown_completes);
    tcase_add_test(tests, test_cli_sd_debounces_swap);
    tcase_add_test(tests, test_cli_sd_initially_absent);
    tcase_add_test(tests, test_cli_sd_removal_during_countdown);
    tcase_add_test(tests, test_cli_sd_from_menu);
    tcase_add_test(tests, test_memory_plan_passes);
    tcase_add_loop_test(tests, test_memory_plan_detects_faults, 0, 2);
    suite_add_tcase(suite, tests);
    return suite;
}

/** Register aborting tests with the process-isolated runner. */
Suite* diag_fatal_suite(void) {
    Suite* const suite = suite_create("diag-fatal");
    TCase* const tests = tcase_create("terminal");
    tcase_add_checked_fixture(tests, setup, teardown);
    tcase_add_test_raise_signal(tests, test_terminal_loop_halts, SIGABRT);
    tcase_add_test_raise_signal(tests, test_cli_sd_prepare_failure, SIGABRT);
    tcase_add_test_raise_signal(tests, test_invalid_plan_structure, SIGABRT);
    suite_add_tcase(suite, tests);
    return suite;
}
