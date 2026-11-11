// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "input/input_actions.h"

/** Execute pending machine intent only when storage is quiet, otherwise return immediately. */
void pet_task(void);

/**
 * Service the IEEE drive until the SD inactivity threshold is met.
 * Leave the CPU state unchanged so running transfers can progress. Continued
 * storage activity extends the wait. This does not close channels, flush
 * caches, or guarantee an empty IEEE transaction pipeline.
 */
void pet_wait_for_storage_idle(void);

// Perform a CBM/PET system reset, including the CPU and IEEE-488 drive. The MCU
// is not affected.
//
// NOTE: Pulling the CPU /RESB pin low independently is unsupported, as this can
//       leave the PET/fabric/firmware in an inconsistent state. Use the RUN pin
//       on the RP2040 to perform a hard reset instead.
void pet_reset();

/**
 * Pulse the PET NMI line.
 *
 * The caller must first complete any pending breakpoint transaction through
 * bp_task(). NMI preserves execution state and must not interleave with the
 * breakpoint manager's temporary patch, resume, restore, and optional rearm.
 */
void pet_nmi(void);
