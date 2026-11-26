// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "diag/diag.h"

/** Destructive diagnostic plan covering blockdevice_sd, FAT, VFS, and stdio. */
extern const diag_plan_t diag_plan_sd;

/** Confirm a destructive card swap and launch max_cycles of diag_plan_sd after the countdown. */
void diag_sd_confirm(uint8_t max_cycles) __attribute__((noreturn));

/** Halt the guest and retire all original-card owners before allowing a swap. */
bool diag_sd_prepare_swap(void);
