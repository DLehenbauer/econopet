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
/** Scratch buffer capacity (including terminating NUL) for formatting dynamic case names. */
#define DIAG_CASE_NAME_MAX 80u

/** Current-iteration state of a diagnostic case. */
typedef enum {
    DIAG_STATUS_WAIT,
    DIAG_STATUS_RUN,
    DIAG_STATUS_PASS,
    DIAG_STATUS_FAIL,
} diag_status_t;

/** One case returns on success or calls diag_fail() from any nested helper. */
typedef struct {
    const char* name;
    void (*run)(const void* context);
} diag_case_t;

typedef struct diag_suite_s diag_suite_t;

/**
 * Group of related diagnostic cases sharing borrowed, plan-lifetime context.
 *
 * Table-backed suites populate cases and use DIAG_SUITE() to wire the default
 * static lookup callbacks. Generated suites may leave cases NULL and supply
 * custom case_name and run_case callbacks indexed from 0 to case_count - 1.
 */
struct diag_suite_s {
    /** Suite heading displayed in the viewport and UART failure summaries. */
    const char* name;
    /** Optional static array of case descriptors (required when using diag_table_* callbacks). */
    const diag_case_t* cases;
    /** Non-zero number of cases in this suite. */
    size_t case_count;
    /** Opaque plan-lifetime context borrowed by the suite's callbacks. */
    const void* context;
    /** Return the display name for index, using caller scratch buf[size] when formatted dynamically. */
    const char* (*case_name)(const diag_suite_t* suite, size_t index, char* buf, size_t size);
    /** Execute case index, returning on pass or calling diag_fail() on mismatch or timeout. */
    void (*run_case)(const diag_suite_t* suite, size_t index);
};

/**
 * Return the static case name at suite->cases[index].
 *
 * Ignores buf and size because table-backed names live in static storage.
 */
const char* diag_table_case_name(const diag_suite_t* suite, size_t index, char* buf, size_t size);

/** Execute suite->cases[index].run with suite->context for a table-backed suite. */
void diag_table_run_case(const diag_suite_t* suite, size_t index);

/** Initialize a static diag_case_t array suite with the default table callbacks. */
#define DIAG_SUITE(suite_name, suite_cases, suite_context)                                                             \
    {                                                                                                                  \
        .name = (suite_name),                                                                                          \
        .cases = (suite_cases),                                                                                        \
        .case_count = count_of(suite_cases),                                                                           \
        .context = (suite_context),                                                                                    \
        .case_name = diag_table_case_name,                                                                             \
        .run_case = diag_table_run_case,                                                                               \
    }

/** CLI-selectable diagnostic plan with optional confirmation, lifecycle hooks, and suites. */
typedef struct {
    /** Exact CLI token accepted by 'test <plan>'. */
    const char* name;
    /** One-line summary printed by CLI help. */
    const char* description;
    /** Optional interactive confirmation gate that forwards max_cycles to diag_run_plan() or resets on abort. */
    void (*confirm)(uint8_t max_cycles) __attribute__((noreturn));
    /**
     * Optional setup after system_takeover() and before session init.
     * Return false with error_out[error_size] populated on recoverable failure.
     * The runner initializes the display, reports the error, and runs cleanup without starting cases.
     */
    bool (*prepare)(char* error_out, size_t error_size);
    /** Optional per-cycle hook run before suite 0 on 1-based iteration 'cycle' (may call diag_fail()). */
    void (*begin_cycle)(uint8_t cycle);
    /** Optional teardown run once when diag_loop() exits; false reports failure before either halt endpoint. */
    bool (*cleanup)(char* error_out, size_t error_size);
    /** Non-empty array of suites executed in order on each cycle. */
    const diag_suite_t* suites;
    /** Number of entries in suites. */
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
    /** Maximum number of cycles to execute in diag_loop() (1..255, defaults to 1). */
    uint8_t max_cycles;
    size_t active_suite_index;
    size_t active_case_index;
    diag_status_t active_status;
    uint16_t case_progress;
    /** True when plan cleanup failed after all executed cases passed. */
    bool cleanup_failed;
    /** True after all max_cycles iterations pass and the completion banner is rendered. */
    bool completed;
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

/** Retain and display a preparation failure before any iteration starts. */
void diag_report_prepare_failure(diag_session_t* session, const char* error);

/** Run one iteration, retaining and reporting the first failure. No hardware preparation. */
bool diag_run_iteration(diag_session_t* session);

/** Refresh elapsed time at most once per second from a long-running case on Core 0. */
void diag_refresh(void);

/** Update the active case progress and render it immediately. */
void diag_case_progress(size_t completed, size_t total);

/**
 * Execute up to session->max_cycles iterations, run plan->cleanup() if provided,
 * and halt via fatal_halt() on failure or diag_complete_halt() on completion.
 * A retained preparation or iteration failure remains terminal.
 */
void diag_loop(diag_session_t* session) __attribute__((noreturn));

/**
 * Park Core 0 in an idle sleep loop after all requested cycles pass, keeping
 * the completed diagnostic display active until reset.
 */
void diag_complete_halt(void) __attribute__((noreturn));

/**
 * Irreversibly take over the system, run plan->prepare(), and execute up to
 * max_cycles iterations of plan until RP2040 reset.
 */
void diag_run_plan(const diag_plan_t* plan, uint8_t max_cycles) __attribute__((noreturn));

/** Fail the active case. Do not use across interrupts or cleanup-requiring resource scopes. */
void diag_fail(const char* format, ...) __attribute__((format(printf, 1, 2), noreturn));

/** Format elapsed microseconds as hours:minutes:seconds (hours may exceed 99). */
void diag_format_elapsed(uint64_t elapsed_us, char* out, size_t out_size);

// Diagnostic expectations remain enabled in deployment builds with NDEBUG.
#define diag_check(cond, fmt, ...)                                                                                     \
    do {                                                                                                               \
        if (!(cond))                                                                                                   \
            diag_fail(fmt, ##__VA_ARGS__);                                                                             \
    } while (0)
