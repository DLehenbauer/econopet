// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag.h"

#include "system/system.h"

// Separate translation unit lets host fixtures intercept the terminal loop after real preparation.
void diag_run_plan(const diag_plan_t* plan) {
    static diag_session_t session;
    system_takeover();
    if (plan->prepare != NULL) {
        plan->prepare();
    }
    diag_session_init(&session, plan);
    printf("DIAG: starting plan '%s' (%zu suite(s))\r\n", plan->name, plan->suite_count);
    fflush(stdout);
    diag_loop(&session);
}
