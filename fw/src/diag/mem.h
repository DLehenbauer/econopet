// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "diag/diag.h"

/** Extended March C- plan covering FPGA-attached SRAM and BRAM. */
extern const diag_plan_t diag_plan_memory;

/** Bring-up plan, initially covering FPGA-attached SRAM and BRAM. */
extern const diag_plan_t diag_plan_bup;

/** Enter bring-up diagnostics if PIA1 does not respond at boot. */
void bup_boot_check(void);
