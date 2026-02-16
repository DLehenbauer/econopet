// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "reset.h"

void system_reset(void) {
    // We use the watchdog to reset the cores and peripherals to get back to
    // a known state before running the firmware.
    watchdog_enable(/* delay_ms: */ 0, /* pause_on_debug: */ true);

    // Wait for watchdog to reset the device
    while (true) {
        __wfi();
    }
}
