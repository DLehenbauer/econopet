// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

/**
 * Performs a system reset using the watchdog timer.
 * 
 * This function uses the RP2040 watchdog to reset all cores and peripherals,
 * returning the system to a known state before running the firmware again.
 * This function never returns.
 */
void __attribute__((noreturn)) system_reset(void);
