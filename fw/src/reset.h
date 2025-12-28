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

#pragma once

/**
 * Performs a system reset using the watchdog timer.
 * 
 * This function uses the RP2040 watchdog to reset all cores and peripherals,
 * returning the system to a known state before running the firmware again.
 * This function never returns.
 */
void __attribute__((noreturn)) system_reset(void);
