// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag/sd.h"

#include <check.h>

static bool (*prepare_handler)(void);

/** Install the fixture's explicit storage takeover behavior. */
void mock_sd_prepare_set_handler(bool (*handler)(void)) {
    prepare_handler = handler;
}

bool diag_sd_prepare_swap(void) {
    ck_assert_msg(prepare_handler != NULL, "unexpected SD swap preparation");
    return prepare_handler();
}

/** Reject executing storage cases in the framework-only fixture. */
static void stub_case(const void* context) {
    (void)context;
    ck_abort_msg("unexpected SD case in diagnostic framework fixture");
}

static const diag_case_t cases[] = {
    { "Host fixture", stub_case },
};
static const diag_suite_t suites[] = {
    DIAG_SUITE("SD host fixture", cases, NULL),
};

const diag_plan_t diag_plan_sd = {
    .name = "sd",
    .description = "Destructive full-stack SD-card diagnostic",
    .confirm = diag_sd_confirm,
    .prepare = NULL,
    .suites = suites,
    .suite_count = count_of(suites),
};
