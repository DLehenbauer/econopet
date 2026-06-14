// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "tape_dir.h"

#include <stdbool.h>
#include <string.h>

#include "cbm/petscii.h"

// CBM directory block size and the maximum value usable as a BASIC line number
// (the line shown for free space). Free bytes are reported in 254-byte blocks.
#define CBM_BLOCK_SIZE 254
#define CBM_MAX_LINE   63999

uint16_t tape_dir_blocks_from_bytes(uint64_t bytes, bool round_up) {
    uint64_t blocks = round_up
        ? (bytes + CBM_BLOCK_SIZE - 1) / CBM_BLOCK_SIZE
        : bytes / CBM_BLOCK_SIZE;

    // A file always occupies at least one block; free space may be zero.
    if (round_up && blocks == 0) {
        blocks = 1;
    }
    if (blocks > CBM_MAX_LINE) {
        blocks = CBM_MAX_LINE;
    }
    return (uint16_t)blocks;
}

// Largest possible line text: lead(3) + '"' + 16 name + '"' + 16 pad + ' '
// + "PRG"(3) = 40 bytes. Round up for headroom.
#define DIR_TEXT_MAX 64

static size_t put_spaces(uint8_t* buf, size_t off, size_t n) {
    while (n--) {
        buf[off++] = ' ';
    }
    return off;
}

// Header line text: RVS ON, the quoted disk name (encoded for the active PET
// character set, padded to 16 columns), then the 2-char disk ID and the 2-char
// DOS format type (e.g. "ECONOPET        " SD 2A).
static size_t build_header_text(uint8_t* t, const char* disk_name, bool graphics_mode) {
    size_t namelen = strlen(disk_name);
    if (namelen > TAPE_DIR_MAX_NAME) {
        namelen = TAPE_DIR_MAX_NAME;
    }

    size_t off = 0;
    t[off++] = PETSCII_RVS_ON;
    t[off++] = PETSCII_QUOTE;
    for (size_t i = 0; i < namelen; i++) {
        t[off++] = ascii_to_petscii((uint8_t)disk_name[i], graphics_mode);
    }
    off = put_spaces(t, off, TAPE_DIR_MAX_NAME - namelen);
    t[off++] = PETSCII_QUOTE;
    t[off++] = ' ';
    t[off++] = 'S';   // disk ID (SD card)
    t[off++] = 'D';
    t[off++] = ' ';
    t[off++] = '2';   // DOS format type
    t[off++] = 'A';
    return off;
}

// File line text: leading spaces (so the quote aligns regardless of the block
// count width), the quoted name (encoded for the active PET character set,
// padded to 16), then "PRG".
static size_t build_file_text(uint8_t* t, const tape_dir_entry_t* e, bool graphics_mode) {
    size_t namelen = strlen(e->name);
    if (namelen > TAPE_DIR_MAX_NAME) {
        namelen = TAPE_DIR_MAX_NAME;
    }

    size_t lead = e->blocks < 10 ? 3 : e->blocks < 100 ? 2 : e->blocks < 1000 ? 1 : 0;

    size_t off = 0;
    off = put_spaces(t, off, lead);
    t[off++] = PETSCII_QUOTE;
    for (size_t i = 0; i < namelen; i++) {
        t[off++] = ascii_to_petscii((uint8_t)e->name[i], graphics_mode);
    }
    t[off++] = PETSCII_QUOTE;
    off = put_spaces(t, off, TAPE_DIR_MAX_NAME - namelen);
    t[off++] = ' ';
    t[off++] = 'P';
    t[off++] = 'R';
    t[off++] = 'G';
    return off;
}

static size_t build_footer_text(uint8_t* t) {
    static const char s[] = "BLOCKS FREE.";
    const size_t len = sizeof(s) - 1;
    memcpy(t, s, len);
    return len;
}

// Emit one BASIC line: link(2) + line number(2) + text + $00. The link points
// at the next line's absolute address (load_addr + new offset).
static size_t emit_line(uint8_t* out, size_t out_cap, size_t off,
                        uint16_t load_addr, uint16_t linenum,
                        const uint8_t* text, size_t text_len, bool* ok) {
    const size_t line_len = 4 + text_len + 1;
    if (off + line_len > out_cap) {
        *ok = false;
        return off;
    }

    const uint16_t line_addr = (uint16_t)(load_addr + off);
    const uint16_t next = (uint16_t)(line_addr + line_len);

    out[off + 0] = (uint8_t)(next & 0xFF);
    out[off + 1] = (uint8_t)(next >> 8);
    out[off + 2] = (uint8_t)(linenum & 0xFF);
    out[off + 3] = (uint8_t)(linenum >> 8);
    memcpy(out + off + 4, text, text_len);
    out[off + 4 + text_len] = 0x00;

    return off + line_len;
}

size_t tape_dir_render(uint8_t* out, size_t out_cap, uint16_t load_addr,
                       const char* disk_name,
                       const tape_dir_entry_t* entries, size_t count,
                       uint64_t free_bytes, bool graphics_mode) {
    uint8_t text[DIR_TEXT_MAX];
    bool ok = true;
    size_t off = 0;
    size_t tlen;

    // Free bytes -> 254-byte CBM blocks (partial blocks dropped, clamped).
    uint16_t free_blocks = tape_dir_blocks_from_bytes(free_bytes, false);

    tlen = build_header_text(text, disk_name, graphics_mode);
    off = emit_line(out, out_cap, off, load_addr, 0, text, tlen, &ok);
    if (!ok) return 0;

    for (size_t i = 0; i < count; i++) {
        if (entries[i].name[0] == '\0') {
            continue;
        }
        tlen = build_file_text(text, &entries[i], graphics_mode);
        off = emit_line(out, out_cap, off, load_addr, entries[i].blocks, text, tlen, &ok);
        if (!ok) return 0;
    }

    tlen = build_footer_text(text);
    off = emit_line(out, out_cap, off, load_addr, free_blocks, text, tlen, &ok);
    if (!ok) return 0;

    // Terminating $0000 link.
    if (off + 2 > out_cap) return 0;
    out[off++] = 0x00;
    out[off++] = 0x00;

    return off;
}
