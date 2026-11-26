// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "system/state.h"

void menu_init(void);
void menu_boot(void);
void menu_enter(void);

/**
 * Apply the named /config.yaml profile for a diagnostic target without mounting
 * user disks, enabling virtual tape, or switching out of diagnostic ownership.
 *
 * Resets the PET with the stock physical or soft 6502 selected and leaves the
 * CPU halted ready for target preparation, returning false if id is not found.
 */
bool menu_apply_diag_config(const char* id);

/**
 * Stream a 2-byte little-endian load-address-prefixed .prg file into PET SRAM
 * below VIDEO_RAM_ADDRESS ($8000) and advance BASIC end-of-program pointers.
 *
 * Returns false if the file cannot be read, has no payload, or reaches $8000.
 */
bool load_prg(const char* filename);
