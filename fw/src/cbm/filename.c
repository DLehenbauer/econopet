// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "cbm/filename.h"
#include "cbm/petscii.h"

#include <string.h>

// Fold a PETSCII byte for case-insensitive comparison: shifted letters
// ($C1-$DA) map onto the unshifted range ($41-$5A) so a name typed in either
// PET character set compares equal.
static uint8_t petscii_fold(uint8_t ch) {
    if (ch >= 0xC1 && ch <= 0xDA) return (uint8_t)(ch & 0x7F);
    return ch;
}

// Encode a host (ASCII) filename character to PETSCII and fold it. This reuses
// the same mapping the directory listing renders with, so the comparison is an
// exact inverse of what the user sees (including relocated punctuation). The
// charset mode is irrelevant here because petscii_fold collapses the letter
// case afterward, so encode without graphics folding (text mode).
static uint8_t encode_fold(char c) {
    return petscii_fold(ascii_to_petscii((uint8_t)c, /* graphics_mode: */ false));
}

bool cbm_filename_match(const uint8_t* pattern, uint8_t pattern_len,
                        const char* filename) {
    if (pattern_len == 0) return false;
    if (pattern_len == 1 && pattern[0] == '*') return false;

    bool has_wildcard = (pattern[pattern_len - 1] == '*');
    uint8_t match_len = has_wildcard ? (uint8_t)(pattern_len - 1) : pattern_len;

    // The filename must be at least as long as the prefix we're matching.
    size_t fname_len = strlen(filename);
    if (fname_len < match_len) return false;

    // Compare the typed PETSCII against the PETSCII-encoded host filename.
    for (uint8_t i = 0; i < match_len; i++) {
        if (petscii_fold(pattern[i]) != encode_fold(filename[i])) return false;
    }

    if (!has_wildcard) {
        // Exact match: filename must be "pattern" or "pattern.prg" (no
        // embedded dots, so "foo" does not match "foo.bar.prg").
        const char* rest = filename + match_len;
        if (rest[0] == '\0') return true;
        static const char ext[] = ".prg";
        // sizeof(ext) includes the terminating NUL; encode_fold('\0') == 0, so
        // the comparison also requires the name to end exactly at ".prg".
        for (uint8_t i = 0; i < sizeof(ext); i++) {
            if (encode_fold(rest[i]) != encode_fold(ext[i])) return false;
        }
        return true;
    }

    return true;
}

