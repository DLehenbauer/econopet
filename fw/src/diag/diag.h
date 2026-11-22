// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display/display.h"

#define DIAG_COLS DISPLAY_COLS_80
#define DIAG_ROWS DISPLAY_ROWS
#define DIAG_MESSAGE_MAX 160u
#define DIAG_SUMMARY_MAX 320u
#define DIAG_PROGRESS_SCALE 1000u

/** Current-iteration state of a diagnostic case. */
typedef enum {
    DIAG_STATUS_WAIT, DIAG_STATUS_RUN, DIAG_STATUS_PASS, DIAG_STATUS_FAIL,
} diag_status_t;

/** One case returns on success or calls diag_fail() from any nested helper. */
typedef struct {
    const char* name;
    void (*run)(const void* context);
} diag_case_t;

/** Cases sharing borrowed, plan-lifetime context. */
typedef struct {
    const char* name;
    const diag_case_t* cases;
    size_t case_count;
    const void* context;
} diag_suite_t;

/** CLI-selectable plan with optional confirmation and hardware preparation. */
typedef struct {
    const char* name;
    const char* description;
    /** Own confirmation until launching the plan or terminating the session. */
    void (*confirm)(void) __attribute__((noreturn));
    void (*prepare)(void);
    const diag_suite_t* suites;
    size_t suite_count;
} diag_plan_t;

/** Fixed-storage runtime for a borrowed plan (Core 0 only, non-reentrant). */
typedef struct {
    const diag_plan_t* plan;
    window_t window;
    uint64_t start_time_us;
    uint64_t last_uart_summary_us;
    uint64_t last_display_refresh_us;
    uint64_t iteration;
    size_t active_suite_index;
    size_t active_case_index;
    diag_status_t active_status;
    uint16_t case_progress;
    jmp_buf fail_jmp;
    bool jmp_armed;
    char failure_detail[DIAG_MESSAGE_MAX];
    char failure_summary[DIAG_SUMMARY_MAX];
} diag_session_t;

/** Find an exact registered CLI plan name, or NULL. */
const diag_plan_t* diag_find_plan(const char* name);
/** Borrow the registered plans and their count for CLI usage. */
const diag_plan_t* const* diag_plans(size_t* count_out);
/** Configure the local display, initialize iteration state, and render the initial screen. */
void diag_session_init(diag_session_t* session, const diag_plan_t* plan);
/** Run one iteration, retaining and reporting the first failure. No hardware preparation. */
bool diag_run_iteration(diag_session_t* session);
/** Refresh elapsed time at most once per second from a long-running case on Core 0. */
void diag_refresh(void);
/** Update the active case progress and render it immediately. */
void diag_case_progress(size_t completed, size_t total);
/** Repeat iterations forever, handing the first failure to display-independent fatal_halt(). */
void diag_loop(diag_session_t* session) __attribute__((noreturn));
/** Irreversibly take over, prepare hardware, and run a plan until RP2040 reset. */
void diag_run_plan(const diag_plan_t* plan) __attribute__((noreturn));
/** Fail the active case. Do not use across interrupts or cleanup-requiring resource scopes. */
void diag_fail(const char* format, ...)
    __attribute__((format(printf, 1, 2), noreturn));
/** Format elapsed microseconds as hours:minutes:seconds (hours may exceed 99). */
void diag_format_elapsed(uint64_t elapsed_us, char* out, size_t out_size);

// Diagnostic expectations remain enabled in deployment builds with NDEBUG.
#define diag_check(cond, fmt, ...) \
    do { if (!(cond)) diag_fail(fmt, ##__VA_ARGS__); } while (0)
