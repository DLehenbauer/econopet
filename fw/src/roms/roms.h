// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#define MENU_ROM_START_ADDRESS 0xFF00

extern const uint8_t rom_chars_e800[0x800];
extern const uint8_t* const p_video_font_000;
extern const uint8_t* const p_video_font_400;

/**
 * Reason for starting the menu ROM. Each entry corresponds to a jump table
 * entry in the menu ROM at $FF00 (see rom/src/main.s).  Each entry is a multiple
 * of 3 bytes, matching the size of a 6502 JMP instruction.
 */
typedef enum {
    /* 0: */ MENU_ROM_BOOT_NORMAL = 0,
    /* 1: */ MENU_ROM_BOOT_ERROR  = 1,
} menu_rom_boot_reason_t;

void start_menu_rom(menu_rom_boot_reason_t reason);
