// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Match a CBM LOAD pattern against a host directory entry name (e.g.
// "game.prg"). The pattern is the raw PETSCII typed by the user (as captured
// from PET memory); the filename is the host ASCII name. Matching is done in
// PETSCII space: each filename character is encoded with the same mapping the
// directory listing uses (ascii_to_petscii), so any glyph the user copies from
// the listing matches, including punctuation that PETSCII relocates. Letters
// compare case-insensitively (the shifted and unshifted forms fold together).
// A trailing '*' requests prefix matching; without it the pattern must equal
// the filename's base, allowing a following ".prg" extension or end of string.
// An empty pattern or a lone "*" never matches (the caller treats those as
// "fall through to the physical datasette").
bool cbm_filename_match(const uint8_t* pattern, uint8_t pattern_len,
                        const char* filename);
