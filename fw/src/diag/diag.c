// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>

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

static const diag_plan_t* const plans[] = {&diag_plan_memory, &diag_plan_sd};
static diag_session_t* active_session;

_Static_assert(DIAG_COLS* DIAG_ROWS <= VIDEO_PLANE_CAPACITY, "diagnostic character plane too small");
_Static_assert(DIAG_COLS* DIAG_ROWS <= VIDEO_COLOR_CAPACITY, "diagnostic color plane too small");

const diag_plan_t* const* diag_plans(size_t* count_out) {
    *count_out = sizeof(plans) / sizeof(plans[0]);
    return plans;
}

const diag_plan_t* diag_find_plan(const char* name) {
    if (name != NULL) {
        for (size_t index = 0; index < sizeof(plans) / sizeof(plans[0]); ++index) {
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

/** Draw one cycle divider at a backward-derived viewport row. */
static void render_cycle_row(const window_t* window, unsigned row, uint64_t cycle_number) {
    const window_t divider = window_with_attr(window, DISPLAY_ATTR(DISPLAY_COLOR_DARK_MAGENTA, DISPLAY_COLOR_BLACK));
    const window_t cycle_win = window_with_attr(window, DISPLAY_ATTR(DISPLAY_COLOR_MAGENTA, DISPLAY_COLOR_BLACK));
    window_fill_row(&divider, row, VROM_HLINE);
    char cycle[DIAG_COLS + 1];
    snprintf(cycle, sizeof(cycle), " Begin Cycle #%" PRIu64 " ", cycle_number);
    window_text(&cycle_win, (DIAG_COLS - (unsigned)strlen(cycle)) / 2 + 1, row, (unsigned)strlen(cycle), cycle);
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
    while (row >= (int)DIAG_BODY_FIRST_ROW) {
        const diag_suite_t* const suite = &session->plan->suites[suite_index];
        render_case_row(window, (unsigned)row, suite->cases[case_index].name,
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
    const bool failed = session->active_status == DIAG_STATUS_FAIL;
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
    window_text(normal, 2, DIAG_LOCATION_ROW, 5, "Suite");
    window_text(&border, 7, DIAG_LOCATION_ROW, 1, ":");
    window_text(&suite_win, 9, DIAG_LOCATION_ROW, DIAG_COLS - 10, suite->name);
    window_text(normal, 3, DIAG_DETAIL_ROW, 4, failed ? "FAIL" : "Case");
    window_text(&border, 7, DIAG_DETAIL_ROW, 1, ":");
    window_text(&case_win, 9, DIAG_DETAIL_ROW, DIAG_COLS - 10,
                failed ? session->failure_detail : suite->cases[session->active_case_index].name);
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
    vet(session != NULL && plan != NULL && plan->name != NULL && plan->suites != NULL && plan->suite_count != 0,
        "invalid diagnostic plan");
    size_t items = 0;
    for (size_t s = 0; s < plan->suite_count; ++s) {
        const diag_suite_t* const suite = &plan->suites[s];
        vet(suite->name != NULL && suite->cases != NULL && suite->case_count != 0 &&
                suite->case_count <= SIZE_MAX - items,
            "invalid diagnostic suite");
        for (size_t c = 0; c < suite->case_count; ++c) {
            vet(suite->cases[c].name != NULL && suite->cases[c].run != NULL, "invalid diagnostic case");
        }
        items += suite->case_count;
    }
    memset(session, 0, sizeof(*session));
    session->plan = plan;
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

/** Keep the jump boundary separate from loops so longjmp cannot clobber their locals. */
static bool run_case(diag_session_t* session, const diag_suite_t* suite, const diag_case_t* test) {
    if (setjmp(session->fail_jmp) != 0) {
        session->jmp_armed = false;
        active_session = NULL;
        return false;
    }
    session->jmp_armed = true;
    active_session = session;
    test->run(suite->context);
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

bool diag_run_iteration(diag_session_t* session) {
    vet(active_session == NULL, "nested diagnostic iteration");
    if (session->active_status == DIAG_STATUS_FAIL) {
        return false;
    }
    ++session->iteration;
    for (size_t s = 0; s < session->plan->suite_count; ++s) {
        const diag_suite_t* const suite = &session->plan->suites[s];
        for (size_t c = 0; c < suite->case_count; ++c) {
            session->active_suite_index = s;
            session->active_case_index = c;
            session->active_status = DIAG_STATUS_RUN;
            session->case_progress = 0;
            render(session);
            if (!run_case(session, suite, &suite->cases[c])) {
                session->active_status = DIAG_STATUS_FAIL;
                char elapsed[DIAG_ELAPSED_SIZE];
                diag_format_elapsed(time_us_64() - session->start_time_us, elapsed, sizeof(elapsed));
                snprintf(session->failure_summary, sizeof(session->failure_summary),
                         "DIAG FAIL [%s] iter %" PRIu64 " (%s) %s / %s: %s", session->plan->name, session->iteration,
                         elapsed, suite->name, suite->cases[c].name, session->failure_detail);
                render(session);
                printf("%s\r\n", session->failure_summary);
                fflush(stdout);
                return false;
            }
            session->active_status = DIAG_STATUS_PASS;
            session->case_progress = DIAG_PROGRESS_SCALE;
            render(session);
        }
    }
    render(session);
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

void diag_loop(diag_session_t* session) {
    while (diag_run_iteration(session)) {
    }
    fatal_halt(FATAL_ERROR_UNSPECIFIED, session->failure_summary);
}
