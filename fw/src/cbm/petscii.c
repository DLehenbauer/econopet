// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "cbm/petscii.h"

#include <ctype.h>

// Convert ASCII/ISO-8859-1 to PETSCII, mirroring the default string mapping
// that the cc65 compiler applies to literals (see cc65's cbm_petscii_charmap.h).
//
// 'fold_case' selects whether to fold upper/lower case letters during mapping.
// Typically set this to true for graphics mode (which has only uppercase
// letters), or when preparing a string for case insensitive comparison (e.g.
// filename matching).
uint8_t ascii_to_petscii(uint8_t ch, bool fold_case) {
    if (fold_case) {
        ch = (uint8_t)tolower(ch);
    }

    // Uppercase A-Z ($41-$5A) -> shifted PETSCII letters ($C1-$DA).
    // Setting the high bit moves them into the shifted half of the set.
    if (ch >= 'A' && ch <= 'Z') {
        return (uint8_t)(ch + 0x80);
    }

    // Lowercase a-z ($61-$7A) -> unshifted PETSCII letters ($41-$5A).
    // Clearing bit 5 ($20) maps them onto the PET's primary letter range.
    if (ch >= 'a' && ch <= 'z') {
        return (uint8_t)(ch - 0x20);
    }

    switch (ch) {
        // Editing control codes reordered to match PETSCII. These form swap
        // pairs, so the inverse mapping is also covered.
        case 0x08: return 0x14;     // backspace       -> delete
        case 0x0A: return 0x0D;     // line feed       -> carriage return
        case 0x0B: return 0x11;     // vertical tab    -> cursor down
        case 0x0C: return 0x93;     // form feed       -> clear screen
        case 0x0D: return 0x0A;     // carriage return -> line feed
        case 0x11: return 0x0B;     // device ctrl 1   -> vertical tab
        case 0x14: return 0x08;     // delete          -> backspace
        case 0x93: return 0x0C;     // clear screen    -> form feed

        // Punctuation that occupies a different code point in PETSCII.
        case 0x5C: return 0xBF;     // backslash
        case 0x5F: return 0xA4;     // underscore
        case 0x60: return 0xAD;     // backtick
        case 0x7B: return 0xB3;     // left brace
        case 0x7C: return 0xDD;     // vertical bar
        case 0x7D: return 0xAB;     // right brace
        case 0x7E: return 0xB1;     // tilde
        case 0x7F: return 0xDF;     // delete glyph

        default:   return ch;       // everything else maps to itself
    }
}

// Convert PETSCII to ASCII. This is the exact inverse of ascii_to_petscii (with
// fold_case = false): letters fold back to their ASCII case, the editing control
// swap pairs reverse, and the relocated punctuation glyphs map back. All other
// codes pass through unchanged.
char petscii_to_ascii(uint8_t ch) {
    // Unshifted PETSCII letters ($41-$5A) hold the lowercase alphabet in cc65's
    // model. Setting bit 5 ($20) yields lowercase ASCII a-z.
    if (ch >= 0x41 && ch <= 0x5A) {
        return (char)(ch | 0x20);
    }

    // Shifted PETSCII letters ($C1-$DA) hold the uppercase alphabet. Clearing
    // the high bit yields uppercase ASCII A-Z. (Only $C1-$DA are produced by
    // the forward map, so the range stops at $DA rather than cc65's $DB.)
    if (ch >= 0xC1 && ch <= 0xDA) {
        return (char)(ch & 0x7F);
    }

    switch (ch) {
        // Editing control codes (swap pairs, identical to the forward map).
        case 0x14: return 0x08;     // delete          -> backspace
        case 0x0D: return 0x0A;     // carriage return -> line feed
        case 0x11: return 0x0B;     // cursor down     -> vertical tab
        case 0x93: return 0x0C;     // clear screen    -> form feed
        case 0x0A: return 0x0D;     // line feed       -> carriage return
        case 0x0B: return 0x11;     // vertical tab    -> device ctrl 1
        case 0x08: return 0x14;     // backspace       -> delete
        case 0x0C: return 0x93;     // form feed       -> clear screen

        // Punctuation relocated by the forward map, mapped back to ASCII.
        case 0xBF: return 0x5C;     // backslash
        case 0xA4: return 0x5F;     // underscore
        case 0xAD: return 0x60;     // backtick
        case 0xB3: return 0x7B;     // left brace
        case 0xDD: return 0x7C;     // vertical bar
        case 0xAB: return 0x7D;     // right brace
        case 0xB1: return 0x7E;     // tilde
        case 0xDF: return 0x7F;     // delete glyph

        default:   return (char)ch; // everything else maps to itself
    }
}
