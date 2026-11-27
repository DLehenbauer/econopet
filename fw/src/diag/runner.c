// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag.h"

#include "fatal.h"
#include "system/system.h"

/** Idle sleep interval while parking Core 0 after a completed diagnostic run. */
#define DIAG_HALT_SLEEP_US 1000000u

/**
 * Park Core 0 in a non-returning sleep loop after all diagnostic cycles pass.
 *
 * Kept in runner.c so host unit tests can wrap diag_complete_halt() with
 * -Wl,--wrap without replacing diag_loop() itself.
 */
void diag_complete_halt(void) {
    // Sleep indefinitely so the completed HDMI summary remains on screen until reset.
    while (true) {
        sleep_us(DIAG_HALT_SLEEP_US);
    }
}

/**
 * Take exclusive diagnostic ownership, prepare hardware, and execute max_cycles
 * of plan in a separate translation unit so host tests can wrap diag_loop().
 */
void diag_run_plan(const diag_plan_t* plan, uint8_t max_cycles) {
    static diag_session_t session;
    char prepare_error[DIAG_MESSAGE_MAX] = "";

    // Claim irreversible diagnostic ownership and run optional hardware/media preparation.
    system_takeover();
    const bool prepared = plan->prepare == NULL || plan->prepare(prepare_error, sizeof(prepare_error));
    vet(system_mode() == system_operation_diagnostic,
        "diagnostic preparation left diagnostic mode");

    // Initialize the diagnostic session with the requested cycle budget and enter the loop.
    diag_session_init(&session, plan, max_cycles);
    if (!prepared) {
        diag_report_prepare_failure(&session, prepare_error);
    } else {
        printf("DIAG: starting plan '%s' (%zu suite(s), %u cycle(s))\r\n", plan->name, plan->suite_count,
               (unsigned)max_cycles);
        fflush(stdout);
    }
    diag_loop(&session);
}
