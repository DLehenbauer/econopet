// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>

#include "diag/drive.h"
#include "diag/sd.h"
#include "display/char_encoding.h"
#include "display/display.h"
#include "display/window.h"
#include "fatal.h"
#include "mem.h"

#define DIAG_BODY_FIRST_ROW 1u
#define DIAG_BODY_LAST_ROW 19u
#define DIAG_DIVIDER_ROW 20u
#define DIAG_LOCATION_ROW 21u
#define DIAG_DETAIL_ROW 22u
#define DIAG_PROGRESS_ROW 23u
#define DIAG_FOOTER_ROW 24u
#define DIAG_NAME_COL 3u
#define DIAG_RESULT_END 78u
#define DIAG_PROGRESS_LABEL_COL 24u
#define DIAG_PROGRESS_END 78u
#define DIAG_US_PER_SECOND UINT64_C(1000000)
#define DIAG_SECONDS_PER_MINUTE 60u
#define DIAG_MINUTES_PER_HOUR 60u
#define DIAG_ELAPSED_SIZE 24u

static const diag_plan_t* const plans[] = {&diag_plan_memory, &diag_plan_bup, &diag_plan_sd, &diag_plan_drive};
static diag_session_t* active_session;

_Static_assert(DIAG_COLS* DIAG_ROWS <= VIDEO_PLANE_CAPACITY, "diagnostic character plane too small");
_Static_assert(DIAG_COLS* DIAG_ROWS <= VIDEO_COLOR_CAPACITY, "diagnostic color plane too small");

/** Return the static name for index from a table-backed suite, ignoring scratch storage. */
const char* diag_table_case_name(const diag_suite_t* suite, size_t index, char* buf, size_t size) {
    // Table-backed suites store string literals directly in suite->cases, so caller scratch is unused.
    (void)buf;
    (void)size;
    return suite->cases[index].name;
}

/** Invoke the static callback for index with the suite's shared context pointer. */
void diag_table_run_case(const diag_suite_t* suite, size_t index) {
    // Dispatch the indexed entry from the static case table with the suite's borrowed context.
    suite->cases[index].run(suite->context);
}

const diag_plan_t* const* diag_plans(size_t* count_out) {
    *count_out = count_of(plans);
    return plans;
}

const diag_plan_t* diag_find_plan(const char* name) {
    if (name != NULL) {
        for (size_t index = 0; index < count_of(plans); ++index) {
            if (strcmp(plans[index]->name, name) == 0) {
                return plans[index];
            }
        }
    }
    return NULL;
}

void diag_format_elapsed(uint64_t elapsed_us, char* out, size_t out_size) {
    const uint64_t seconds = elapsed_us / DIAG_US_PER_SECOND;
    const uint64_t minutes = seconds / DIAG_SECONDS_PER_MINUTE;
    snprintf(out, out_size, "%02" PRIu64 ":%02u:%02u", minutes / DIAG_MINUTES_PER_HOUR,
             (unsigned)(minutes % DIAG_MINUTES_PER_HOUR), (unsigned)(seconds % DIAG_SECONDS_PER_MINUTE));
}

/** Draw one case at a backward-derived viewport row. */
static void render_case_row(const window_t* window, unsigned row, const char* case_name, diag_status_t status) {
    const window_t dots = window_with_attr(window, DISPLAY_ATTR(DISPLAY_COLOR_MEDIUM_GRAY, DISPLAY_COLOR_BLACK));
    const window_t case_win =
        window_with_attr(window, DISPLAY_ATTR(status == DIAG_STATUS_RUN ? DISPLAY_COLOR_YELLOW : DISPLAY_COLOR_WHITE,
                                              DISPLAY_COLOR_BLACK));
    const char* const result = status == DIAG_STATUS_RUN ? "RUN" : status == DIAG_STATUS_FAIL ? "FAIL" : "OK";
    const window_t result_win =
        window_with_attr(window, DISPLAY_ATTR(status == DIAG_STATUS_RUN    ? DISPLAY_COLOR_YELLOW
                                              : status == DIAG_STATUS_FAIL ? DISPLAY_COLOR_LIGHT_RED
                                                                           : DISPLAY_COLOR_GREEN,
                                              DISPLAY_COLOR_BLACK));
    window_fill_row(window, row, VROM_SPACE);
    window_hline_at(&dots, DIAG_NAME_COL, row, DIAG_RESULT_END - DIAG_NAME_COL, ascii_to_vrom('.'));
    window_text(&case_win, DIAG_NAME_COL, row, DIAG_RESULT_END - DIAG_NAME_COL - 4, case_name);
    window_text(&result_win, DIAG_RESULT_END - (unsigned)strlen(result), row, (unsigned)strlen(result), result);
}

/** Draw one suite heading at a backward-derived viewport row. */
static void render_suite_row(const window_t* window, unsigned row, const char* suite_name) {
    const window_t suite_win = window_with_attr(window, DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_CYAN, DISPLAY_COLOR_BLACK));
    window_fill_row(window, row, VROM_SPACE);
    window_text(&suite_win, 1, row, DIAG_COLS - 2, suite_name);
}

/** Draw one centered magenta divider banner over a full-width horizontal rule at row. */
static void render_divider_row(const window_t* window, unsigned row, const char* text) {
    // Build distinct color views so the rule stays dim while the centered title stands out.
    const window_t divider = window_with_attr(window, DISPLAY_ATTR(DISPLAY_COLOR_DARK_MAGENTA, DISPLAY_COLOR_BLACK));
    const window_t text_win = window_with_attr(window, DISPLAY_ATTR(DISPLAY_COLOR_MAGENTA, DISPLAY_COLOR_BLACK));
    const unsigned length = (unsigned)strlen(text);

    // Fill the entire interior width with a horizontal line, then overwrite the centered label.
    window_fill_row(&divider, row, VROM_HLINE);
    window_text(&text_win, (DIAG_COLS - length) / 2 + 1, row, length, text);
}

/** Draw one cycle divider at a backward-derived viewport row. */
static void render_cycle_row(const window_t* window, unsigned row, uint64_t cycle_number) {
    // Format the 1-based cycle label and render it as a centered divider row.
    char cycle[DIAG_COLS + 1];
    snprintf(cycle, sizeof(cycle), " Begin Cycle #%" PRIu64 " ", cycle_number);
    render_divider_row(window, row, cycle);
}

/** Reconstruct only the visible 19-row history from the active cursor. */
static void render_body(const diag_session_t* session) {
    const window_t* const window = &session->window;
    if (session->iteration == 0) {
        for (unsigned row = DIAG_BODY_FIRST_ROW; row <= DIAG_BODY_LAST_ROW; ++row) {
            window_fill_row(window, row, VROM_SPACE);
        }
        return;
    }

    uint64_t cycle = session->iteration;
    size_t suite_index = session->active_suite_index;
    size_t case_index = session->active_case_index;
    bool is_active = true;
    int row = (int)DIAG_BODY_LAST_ROW;

    // Reserve the bottom viewport row for a completion banner once all requested cycles pass.
    if (session->completed) {
        render_divider_row(window, (unsigned)row--, " All Cycles Completed ");
    }

    // Walk backward from the active case, resolving each static or generated case name on demand.
    char name_buf[DIAG_CASE_NAME_MAX];
    while (row >= (int)DIAG_BODY_FIRST_ROW) {
        const diag_suite_t* const suite = &session->plan->suites[suite_index];
        const char* const case_name = suite->case_name(suite, case_index, name_buf, sizeof(name_buf));
        render_case_row(window, (unsigned)row, case_name,
                        is_active ? session->active_status : DIAG_STATUS_PASS);
        is_active = false;
        --row;
        if (case_index != 0) {
            --case_index;
            continue;
        }
        if (row >= (int)DIAG_BODY_FIRST_ROW) {
            render_suite_row(window, (unsigned)row, suite->name);
            --row;
        }
        if (suite_index != 0) {
            --suite_index;
            case_index = session->plan->suites[suite_index].case_count - 1;
            continue;
        }
        if (row >= (int)DIAG_BODY_FIRST_ROW) {
            render_cycle_row(window, (unsigned)row, cycle);
            --row;
        }
        if (cycle > 1) {
            --cycle;
            suite_index = session->plan->suite_count - 1;
            case_index = session->plan->suites[suite_index].case_count - 1;
            continue;
        }
        while (row >= (int)DIAG_BODY_FIRST_ROW) {
            window_fill_row(window, (unsigned)row--, VROM_SPACE);
        }
        break;
    }
}

/** Count complete cycles directly from the current active cursor. */
static uint64_t completed_iterations(const diag_session_t* session) {
    if (session->iteration == 0) {
        return 0;
    }
    const size_t last_suite = session->plan->suite_count - 1;
    const bool at_final_case = session->active_suite_index == last_suite &&
                               session->active_case_index == session->plan->suites[last_suite].case_count - 1 &&
                               session->active_status == DIAG_STATUS_PASS;
    return at_final_case ? session->iteration : session->iteration - 1;
}

/** Refresh the fixed header and footer while preserving the scrolling event history. */
static void render(const diag_session_t* session) {
    const window_t* const normal = &session->window;
    const window_t border = window_with_attr(normal, DISPLAY_ATTR(DISPLAY_COLOR_MEDIUM_GRAY, DISPLAY_COLOR_BLACK));
    const window_t suite_win = window_with_attr(normal, DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_CYAN, DISPLAY_COLOR_BLACK));
    const bool cleanup_failed = session->cleanup_failed;
    const bool failed = session->active_status == DIAG_STATUS_FAIL || cleanup_failed;
    const bool preparation_failed = session->active_status == DIAG_STATUS_FAIL && session->iteration == 0;
    const bool stage_failed = preparation_failed || cleanup_failed;
    const window_t case_win = window_with_attr(
        normal, DISPLAY_ATTR(failed ? DISPLAY_COLOR_LIGHT_RED : DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_BLACK));
    char elapsed[DIAG_ELAPSED_SIZE], progress[DIAG_COLS + 1];
    diag_format_elapsed(time_us_64() - session->start_time_us, elapsed, sizeof(elapsed));
    snprintf(progress, sizeof(progress), "|   Completed: %" PRIu64 " | Time: %s ", completed_iterations(session),
             elapsed);
    render_body(session);

    // Bound the title by the right-aligned progress, including very long runtimes.
    window_fill_row(normal, 0, VROM_SPACE);
    const unsigned progress_col = DIAG_COLS - (unsigned)strlen(progress);
    const char* const name = session->plan->name;
    window_printf(normal, 0, 0, progress_col, " EconoPET %c%s Diagnostic",
                  name[0] == '\0' ? ' ' : toupper((unsigned char)name[0]), name[0] == '\0' ? "" : name + 1);
    window_text(normal, progress_col, 0, DIAG_COLS - progress_col, progress);
    for (unsigned index = 0; progress[index] != '\0'; ++index) {
        if (progress[index] == '|') {
            window_put(normal, progress_col + index, 0, VROM_VLINE);
        }
    }
    window_put(normal, progress_col + 2, 0, VROM_CHECKMARK);
    window_reverse_at(normal, 0, 0, DIAG_COLS);

    // Clear the interior first so the frame retains its vertical borders.
    for (unsigned row = DIAG_LOCATION_ROW; row <= DIAG_PROGRESS_ROW; ++row) {
        window_fill_row(normal, row, VROM_SPACE);
    }
    window_frame(&border, 0, DIAG_DIVIDER_ROW, DIAG_COLS, DIAG_FOOTER_ROW - DIAG_DIVIDER_ROW + 1, NULL);
    const diag_suite_t* const suite = &session->plan->suites[session->active_suite_index];

    // Show the recorded failure detail on error, or resolve the active case's display label.
    char active_name_buf[DIAG_CASE_NAME_MAX];
    const char* const active_case_name =
        failed ? session->failure_detail
               : suite->case_name(suite, session->active_case_index, active_name_buf, sizeof(active_name_buf));
    window_text(normal, 2, DIAG_LOCATION_ROW, 5, stage_failed ? "Stage" : "Suite");
    window_text(&border, 7, DIAG_LOCATION_ROW, 1, ":");
    window_text(&suite_win, 9, DIAG_LOCATION_ROW, DIAG_COLS - 10,
                preparation_failed ? "Preparation" : cleanup_failed ? "Cleanup" : suite->name);
    window_text(normal, 3, DIAG_DETAIL_ROW, 4, failed ? "FAIL" : "Case");
    window_text(&border, 7, DIAG_DETAIL_ROW, 1, ":");
    window_text(&case_win, 9, DIAG_DETAIL_ROW, DIAG_COLS - 10, active_case_name);
    char cycle_number[DIAG_ELAPSED_SIZE];
    snprintf(cycle_number, sizeof(cycle_number), "%" PRIu64, session->iteration);
    const unsigned cycle_width = (unsigned)strlen(cycle_number);
    const unsigned label_col =
        9u + cycle_width + 1u > DIAG_PROGRESS_LABEL_COL ? 9u + cycle_width + 1u : DIAG_PROGRESS_LABEL_COL;
    const unsigned bar_col = label_col + 10u;
    const unsigned bar_width = DIAG_PROGRESS_END - bar_col;
    window_text(normal, 2, DIAG_PROGRESS_ROW, 5, "Cycle");
    window_text(&border, 7, DIAG_PROGRESS_ROW, 1, ":");
    window_text(normal, 9, DIAG_PROGRESS_ROW, cycle_width, cycle_number);
    window_text(normal, label_col, DIAG_PROGRESS_ROW, 8, "Progress");
    window_text(&border, label_col + 8, DIAG_PROGRESS_ROW, 1, ":");
    size_t total_items = 0;
    size_t preceding_items = 0;
    for (size_t index = 0; index < session->plan->suite_count; ++index) {
        total_items += session->plan->suites[index].case_count;
        if (index < session->active_suite_index) {
            preceding_items += session->plan->suites[index].case_count;
        } else if (index == session->active_suite_index) {
            preceding_items += session->active_case_index;
        }
    }
    const uint64_t done =
        session->iteration == 0 ? 0 : (uint64_t)preceding_items * DIAG_PROGRESS_SCALE + session->case_progress;
    const unsigned filled = (unsigned)((done * bar_width) / ((uint64_t)total_items * DIAG_PROGRESS_SCALE));
    const window_t filled_win = window_with_attr(normal, DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_BLUE, DISPLAY_COLOR_BLACK));
    const window_t empty_win = window_with_attr(normal, DISPLAY_ATTR(DISPLAY_COLOR_BLUE, DISPLAY_COLOR_BLACK));
    window_hline_at(&filled_win, bar_col, DIAG_PROGRESS_ROW, filled, VROM_REVERSE_SPACE);
    window_hline_at(&empty_win, bar_col + filled, DIAG_PROGRESS_ROW, bar_width - filled, VROM_SHADE);
}

void diag_session_init(diag_session_t* session, const diag_plan_t* plan) {
    // Validate plan and suite callback invariants up front, checking static tables when used.
    vet(session != NULL && plan != NULL && plan->name != NULL && plan->suites != NULL && plan->suite_count != 0,
        "invalid diagnostic plan");
    size_t items = 0;
    for (size_t s = 0; s < plan->suite_count; ++s) {
        const diag_suite_t* const suite = &plan->suites[s];
        vet(suite->name != NULL && suite->case_count != 0 && suite->case_name != NULL && suite->run_case != NULL &&
                suite->case_count <= SIZE_MAX - items,
            "invalid diagnostic suite");
        if (suite->case_name == diag_table_case_name || suite->run_case == diag_table_run_case) {
            vet(suite->cases != NULL, "missing diagnostic case table");
            for (size_t c = 0; c < suite->case_count; ++c) {
                vet(suite->cases[c].name != NULL && suite->cases[c].run != NULL, "invalid diagnostic case");
            }
        }
        items += suite->case_count;
    }

    // Reset session state to a single-cycle default and initialize the 80-column diagnostic viewport.
    memset(session, 0, sizeof(*session));
    session->plan = plan;
    session->max_cycles = 1;
    session->start_time_us = time_us_64();
    session->last_display_refresh_us = session->start_time_us;
    session->window =
        display_init_local(pet_display_columns_80, DISPLAY_ATTR(DISPLAY_COLOR_LIGHT_GRAY, DISPLAY_COLOR_BLACK));
    render(session);
}

void diag_refresh(void) {
    assert(active_session != NULL && active_session->jmp_armed);
    const uint64_t now = time_us_64();
    if (now - active_session->last_display_refresh_us >= DIAG_US_PER_SECOND) {
        active_session->last_display_refresh_us = now;
        render(active_session);
    }
}

void diag_case_progress(size_t completed, size_t total) {
    vet(active_session != NULL && active_session->jmp_armed, "diagnostic progress outside an active case");
    diag_check(total != 0, "invalid diagnostic progress total");
    const uint64_t bounded = completed > total ? total : completed;
    active_session->case_progress = (uint16_t)(bounded * DIAG_PROGRESS_SCALE / total);
    render(active_session);
}

/**
 * Invoke the plan's optional per-cycle hook inside its own setjmp frame so
 * diag_fail() can abort setup without clobbering caller loop variables.
 */
static bool run_begin_cycle(diag_session_t* session, uint8_t cycle) {
    // Succeed immediately when the plan requires no per-cycle trigger.
    if (session->plan->begin_cycle == NULL) {
        return true;
    }

    // Disarm the failure jump buffer if begin_cycle unwinds via diag_fail().
    if (setjmp(session->fail_jmp) != 0) {
        session->jmp_armed = false;
        active_session = NULL;
        return false;
    }

    // Arm the active session around the hook so deferred setup checks can call diag_fail().
    session->jmp_armed = true;
    active_session = session;
    session->plan->begin_cycle(cycle);
    session->jmp_armed = false;
    active_session = NULL;
    return true;
}

/**
 * Execute one suite case inside an isolated setjmp frame so longjmp cannot
 * clobber the outer suite and case loop indices.
 */
static bool run_case(diag_session_t* session, const diag_suite_t* suite, size_t case_index) {
    // Disarm the failure jump buffer when the case or a helper unwinds via diag_fail().
    if (setjmp(session->fail_jmp) != 0) {
        session->jmp_armed = false;
        active_session = NULL;
        return false;
    }

    // Arm the active session around the suite's case runner and disarm on normal return.
    session->jmp_armed = true;
    active_session = session;
    suite->run_case(suite, case_index);
    session->jmp_armed = false;
    active_session = NULL;
    return true;
}

void diag_fail(const char* format, ...) {
    vet(active_session != NULL && active_session->jmp_armed, "diagnostic failure outside an active case");
    va_list args;
    va_start(args, format);
    vsnprintf(active_session->failure_detail, sizeof(active_session->failure_detail), format, args);
    va_end(args);
    longjmp(active_session->fail_jmp, 1);
}

/**
 * Latch DIAG_STATUS_FAIL on the session, format the one-line UART failure
 * summary for (suite, case_index), and render the failed viewport.
 */
static bool report_iteration_failure(diag_session_t* session, const diag_suite_t* suite, size_t case_index) {
    // Transition the active cursor to FAIL and resolve elapsed time plus the case's display name.
    session->active_status = DIAG_STATUS_FAIL;
    char elapsed[DIAG_ELAPSED_SIZE];
    char name_buf[DIAG_CASE_NAME_MAX];
    diag_format_elapsed(time_us_64() - session->start_time_us, elapsed, sizeof(elapsed));
    const char* const case_name = suite->case_name(suite, case_index, name_buf, sizeof(name_buf));

    // Format the terminal failure summary, update the HDMI screen, and emit the UART report.
    snprintf(session->failure_summary, sizeof(session->failure_summary),
             "DIAG FAIL [%s] iter %" PRIu64 " (%s) %s / %s: %s", session->plan->name, session->iteration, elapsed,
             suite->name, case_name, session->failure_detail);
    render(session);
    printf("%s\r\n", session->failure_summary);
    fflush(stdout);
    return false;
}

void diag_report_prepare_failure(diag_session_t* session, const char* error) {
    vet(session->iteration == 0 && active_session == NULL, "preparation failure after diagnostic start");
    session->active_status = DIAG_STATUS_FAIL;
    snprintf(session->failure_detail, sizeof(session->failure_detail), "%s",
             error[0] != '\0' ? error : "diagnostic preparation failed");
    snprintf(session->failure_summary, sizeof(session->failure_summary), "DIAG FAIL [%s] preparation: %s",
             session->plan->name, session->failure_detail);
    render(session);
    printf("%s\r\n", session->failure_summary);
    fflush(stdout);
}

bool diag_run_iteration(diag_session_t* session) {
    vet(active_session == NULL, "nested diagnostic iteration");
    if (session->active_status == DIAG_STATUS_FAIL) {
        return false;
    }

    // Advance the cycle counter, prime the cursor at the first case, and run per-cycle setup.
    ++session->iteration;
    session->completed = false;
    session->active_suite_index = 0;
    session->active_case_index = 0;
    session->active_status = DIAG_STATUS_RUN;
    session->case_progress = 0;
    if (session->plan->begin_cycle != NULL) {
        render(session);
    }
    if (!run_begin_cycle(session, (uint8_t)session->iteration)) {
        return report_iteration_failure(session, &session->plan->suites[0], 0);
    }

    // Execute every case in every suite, stopping immediately on the first failure.
    for (size_t s = 0; s < session->plan->suite_count; ++s) {
        const diag_suite_t* const suite = &session->plan->suites[s];
        for (size_t c = 0; c < suite->case_count; ++c) {
            session->active_suite_index = s;
            session->active_case_index = c;
            session->active_status = DIAG_STATUS_RUN;
            session->case_progress = 0;
            render(session);
            if (!run_case(session, suite, c)) {
                return report_iteration_failure(session, suite, c);
            }
            session->active_status = DIAG_STATUS_PASS;
            session->case_progress = DIAG_PROGRESS_SCALE;
            render(session);
        }
    }
    const uint64_t now = time_us_64();
    if (session->iteration == 1 || now - session->last_uart_summary_us >= DIAG_US_PER_SECOND) {
        char elapsed[DIAG_ELAPSED_SIZE];
        diag_format_elapsed(now - session->start_time_us, elapsed, sizeof(elapsed));
        printf("DIAG PASS [%s] iter #%" PRIu64 " (%s)\r\n", session->plan->name, session->iteration, elapsed);
        fflush(stdout);
        session->last_uart_summary_us = now;
    }
    return true;
}

/** Run up to session->max_cycles iterations, clean up plan resources, and halt on pass or fail. */
void diag_loop(diag_session_t* session) {
    // Run iterations until max_cycles complete or an iteration fails.
    vet(session->max_cycles != 0, "diagnostic cycle count must be non-zero");
    bool passed = session->active_status != DIAG_STATUS_FAIL;
    while (passed && session->iteration < session->max_cycles) {
        if (!diag_run_iteration(session)) {
            passed = false;
            break;
        }
    }

    // Release temporary plan resources (such as mounted scratch media) before halting.
    if (session->plan->cleanup != NULL) {
        char cleanup_error[DIAG_MESSAGE_MAX] = "";
        if (!session->plan->cleanup(cleanup_error, sizeof(cleanup_error))) {
            const char* const detail = cleanup_error[0] != '\0' ? cleanup_error : "diagnostic cleanup failed";
            if (passed) {
                session->cleanup_failed = true;
                snprintf(session->failure_detail, sizeof(session->failure_detail), "%s", detail);
                snprintf(session->failure_summary, sizeof(session->failure_summary),
                         "DIAG FAIL [%s] cleanup: %s", session->plan->name, detail);
                render(session);
                printf("%s\r\n", session->failure_summary);
            } else {
                printf("DIAG CLEANUP FAIL [%s]: %s\r\n", session->plan->name, detail);
            }
            fflush(stdout);
            passed = false;
        }
    }

    // Escalate failures to the Morse/UART fatal endpoint while keeping the failure screen visible.
    if (!passed) {
        fatal_halt(FATAL_ERROR_UNSPECIFIED, session->failure_summary);
    }

    // Render the final completion divider, emit the UART completion line, and park Core 0.
    session->completed = true;
    render(session);
    char elapsed[DIAG_ELAPSED_SIZE];
    diag_format_elapsed(time_us_64() - session->start_time_us, elapsed, sizeof(elapsed));
    printf("DIAG COMPLETE [%s] %" PRIu64 " cycle(s) passed (%s)\r\n",
           session->plan->name, session->iteration, elapsed);
    fflush(stdout);
    diag_complete_halt();
}
