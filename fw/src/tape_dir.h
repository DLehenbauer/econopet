// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum number of characters shown for a directory entry name (the CBM
// filename field is 16 characters wide).
#define TAPE_DIR_MAX_NAME 16

// Convert a byte count to CBM 254-byte blocks, clamped to 63999 (the maximum
// CBM BASIC line number). When 'round_up' is true, partial blocks count as a
// full block and the result is at least 1 (used for file sizes). When false,
// partial blocks are dropped and zero is allowed (used for free space).
uint16_t tape_dir_blocks_from_bytes(uint64_t bytes, bool round_up);

// A single loadable entry in the synthesized directory listing. The name has
// the '.prg' extension already suppressed.
typedef struct {
    char name[TAPE_DIR_MAX_NAME + 1];
    uint16_t blocks;        // ceil(file_size / 254)
} tape_dir_entry_t;

// Render a Commodore-style directory listing as a BASIC program image that
// loads at 'load_addr' (typically $0401). Writes the program bytes (with no
// PRG 2-byte load-address prefix, since the image is copied straight to SRAM)
// into 'out'.
//
// 'graphics_mode' selects how entry names are encoded for the PET's active
// character set:
//   - true  (uppercase/graphics set): names are folded so every letter lands
//     in the $41-$5A range, which displays as uppercase (lowercase glyphs do
//     not exist in this set).
//   - false (lowercase/business set): the host filename's case is preserved,
//     so mixed-case names display faithfully.
//
// The image contains:
//   - A header line (line number 0) showing 'disk_name' as the quoted disk
//     name (encoded per 'graphics_mode', padded to 16 columns), followed by
//     the 2-char disk ID and the '2A' DOS format type.
//   - One line per entry: block count as the line number, the quoted name
//     (encoded per 'graphics_mode', '.prg' suppressed), then 'PRG'.
//   - A footer line: 'free_blocks' as the line number, then 'BLOCKS FREE.'.
//
// 'free_bytes' is the SD card free space in bytes, converted internally to
// 254-byte CBM blocks (clamped to the CBM line number maximum).
//
// Line link pointers are filled in with their correct absolute addresses based
// on 'load_addr'. The image is terminated with a $0000 link.
//
// Returns the number of bytes written, or 0 if the image would exceed
// 'out_cap'.
size_t tape_dir_render(uint8_t* out, size_t out_cap, uint16_t load_addr,
                       const char* disk_name,
                       const tape_dir_entry_t* entries, size_t count,
                       uint64_t free_bytes, bool graphics_mode);
