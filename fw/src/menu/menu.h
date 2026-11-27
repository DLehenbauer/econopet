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
 * Returns false if the profile ID is absent. Required ROM file errors are fatal.
 *
 * Selects the stock physical or soft 6502 and leaves it halted without resetting.
 * The caller must reset the PET after mounting any diagnostic media.
 */
bool menu_apply_diag_config(const char* id);

/**
 * Stream a 2-byte little-endian load-address-prefixed .prg file into PET SRAM
 * below VIDEO_RAM_ADDRESS ($8000) and advance BASIC end-of-program pointers.
 *
 * Returns false if the file cannot be read, has no payload, or writes at $8000+.
 * On failure the BASIC end pointers are unchanged, but earlier chunks may have
 * been written. Callers must keep the CPU halted and must not run a failed load.
 */
bool load_prg(const char* filename);

/**
 * Load a PRG using an exclusive SRAM end_limit (at most $8000), reserving memory
 * at and above that address. Otherwise has the same behavior as load_prg().
 */
bool load_prg_bounded(const char* filename, uint16_t end_limit);
