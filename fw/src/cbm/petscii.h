// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

// PETSCII control codes.
#define PETSCII_RVS_ON 0x12     // Reverse video on
#define PETSCII_QUOTE  0x22     // Double quote

// Convert an ASCII/ISO-8859-1 byte to PETSCII, mirroring the cc65 compiler's
// default literal mapping (cbm_petscii_charmap.h). Letters swap between the
// PET's unshifted and shifted character sets, a few editing control codes are
// reordered, and a handful of punctuation glyphs are relocated. All other
// codes pass through unchanged.
//
// 'fold_case' folds letters to lowercase before mapping. Set it for graphics
// mode (which has only uppercase letters, so both cases must land in $41-$5A)
// or when preparing a string for case-insensitive comparison. Leave it clear in
// text mode to preserve the source case (uppercase -> shifted $C1-$DA) for a
// faithful mixed-case display.
uint8_t ascii_to_petscii(uint8_t ch, bool fold_case);

// Convert a PETSCII byte to ASCII. This is the exact inverse of ascii_to_petscii
// (with fold_case = false): letters fold back to their ASCII case ($41-$5A ->
// lowercase, $C1-$DA -> uppercase), the editing control swap pairs reverse, and
// the relocated punctuation glyphs map back. All other codes pass through
// unchanged.
char petscii_to_ascii(uint8_t ch);
