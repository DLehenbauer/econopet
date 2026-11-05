// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

// Perform a CBM/PET system reset, including the CPU and IEEE-488 drive. The MCU
// is not affected.
//
// NOTE: Pulling the CPU /RESB pin low independently is unsupported, as this can
//       leave the PET/fabric/firmware in an inconsistent state. Use the RUN pin
//       on the RP2040 to perform a hard reset instead.
void pet_reset();

void pet_nmi();
