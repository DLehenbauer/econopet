/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

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
