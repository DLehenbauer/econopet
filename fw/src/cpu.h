// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Detect the socketed CPU once after bp_init(), before loading any configuration.
// Requires a halted/reset CPU. Overwrites $0400-$040C, $0410 and $FFFC-$FFFD.
// Leaves the detected physical or fallback soft 6502 selected and held in reset.
void cpu_init(void);

// Return the startup detection result without touching hardware. Requires cpu_init().
bool physical_cpu_present(void);
