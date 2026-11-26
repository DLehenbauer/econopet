// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

/** Committed top-level execution mode. Terminal routing remains independent. */
typedef enum {
    /** Firmware is completing startup and the boot configuration picker is not yet committed. */
    system_operation_boot,
    /** The configured PET guest owns normal machine execution and its input paths. */
    system_operation_guest,
    /** Firmware has suspended the guest and owns its bounded console resources. */
    system_operation_console,
    /** Firmware is in a destructive, non-returning diagnostic or fatal path until power cycle. */
    system_operation_diagnostic,
} system_operation_t;

/** Initialize operation ownership for firmware startup or a host fixture. */
void system_init(void);
/** Return the committed operation mode. */
system_operation_t system_mode(void);
/**
 * Borrow the guest's bounded resources for a firmware console, rejecting nested entry.
 *
 * This is not a peripheral snapshot. Shared rev-a clocks and external inputs keep
 * running. PIA1 data reads acknowledge cassette/retrace flags, which are not
 * reconstructed. Active cassette transfers are outside the resume guarantee.
 * Unborrowed VIA/PIA2 interrupt state is left alone.
 */
bool system_suspend(void);
/** Restore borrowed resources, optionally returning CPU control to the guest. */
void system_restore(bool resume);
/** Reset into the guest after the caller has completed destructive teardown. */
void system_reset_guest(void);
/**
 * Park physical VIA and PIA2 IEEE output pins as inputs while the CPU is
 * halted (pre-charging IEEE output latches high first and leaving non-IEEE
 * VIA directions at their 0x00 reset state until guest DDRB writes sync).
 */
void system_park_ieee_io(void);
/**
 * Lazily forward non-IEEE guest VIA DDRB updates snooped in $7000F to physical
 * VIA when virtual IEEE emulation is active in guest mode.
 */
void system_sync_ieee_io(void);
/**
 * Irreversibly abandon normal operation and any resumable guest context.
 *
 * Publishes diagnostic ownership before console teardown begins. The caller must
 * immediately enter a terminal operation. This helper returns and does not stop
 * core 1, interrupts, or peripherals on its own.
 */
void system_takeover(void);
/** Take over system operation and reset the RP2040 through its watchdog. */
void __attribute__((noreturn)) system_reset(void);
/** Run tasks admitted only while the guest is active. */
void system_task(void);
