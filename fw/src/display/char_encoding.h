// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

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
 * - menu_ui.c and diag.c: For shared display glyphs
 * - display.c: For terminal output to VT-100 compatible terminals
 */

/** PET VROM reverse-video bit and shared screen glyphs. */
#define VROM_REVERSE_MASK 0x80u
#define VROM_SPACE 0x20u
#define VROM_REVERSE_SPACE (VROM_SPACE | VROM_REVERSE_MASK)
#define VROM_HLINE 0x40u
#define VROM_VLINE 0x5du
#define VROM_CORNER_TOP_LEFT 0x70u
#define VROM_CORNER_TOP_RIGHT 0x6eu
#define VROM_CORNER_BOTTOM_LEFT 0x6du
#define VROM_CORNER_BOTTOM_RIGHT 0x7du
#define VROM_EDGE_LEFT 0x61u
#define VROM_UNDERLINE 0x63u
#define VROM_SHADE 0x66u
#define VROM_CAP_TOP 0x71u
#define VROM_TEE_DOWN 0x72u
#define VROM_LINE_RIGHT 0x74u
#define VROM_EDGE_RIGHT 0x76u
#define VROM_CHECKMARK 0x7au

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
    return vrom_to_term_map[vrom & (uint8_t)~VROM_REVERSE_MASK];
}
