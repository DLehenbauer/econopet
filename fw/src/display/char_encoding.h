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

#include <stdint.h>

/**
 * @file char_encoding.h
 * @brief Character encoding conversion tables for PET video display.
 *
 * This module provides the canonical tables for converting between:
 * - ASCII characters and PET Video ROM (VROM) offsets
 * - PET VROM offsets and VT-100 terminal escape sequences
 *
 * These tables are used by:
 * - window.c: For rendering ASCII text to the character buffer
 * - menu.c: For screen display functions
 * - term.c: For terminal output to VT-100 compatible terminals
 */

/**
 * Maps ASCII characters (0x00-0x7F) to PET Video ROM offsets.
 * Used for lower-case character set (POKE 59468,14).
 *
 * Index: ASCII character code (0-127)
 * Value: Corresponding PET VROM offset
 */
extern const uint8_t ascii_to_vrom_map[128];

/**
 * Maps PET Video ROM offsets (0x00-0x7F) to VT-100 terminal strings.
 * Used for rendering PET characters on a VT-100 compatible terminal.
 *
 * Index: PET VROM offset (0-127)
 * Value: Pointer to null-terminated string (may contain escape sequences)
 *
 * Note: Some PET graphics characters use VT-100 line drawing mode:
 *   \e(0 enters line drawing mode
 *   \e(B returns to normal mode
 */
extern const char* const vrom_to_term_map[128];

/**
 * Convert an ASCII character to a PET VROM offset.
 *
 * @param ascii ASCII character code (0-127)
 * @return PET VROM offset, or 0 for out-of-range input
 */
static inline uint8_t ascii_to_vrom(uint8_t ascii) {
    return (ascii < 128) ? ascii_to_vrom_map[ascii] : 0;
}

/**
 * Convert a PET VROM offset to a VT-100 terminal string.
 *
 * @param vrom PET VROM offset (0-127, high bit ignored)
 * @return Pointer to terminal string representation
 */
static inline const char* vrom_to_term(uint8_t vrom) {
    return vrom_to_term_map[vrom & 0x7F];
}
