// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#define MENU_ROM_START_ADDRESS 0xFF00

// Character ROM base in FPGA SRAM (see common_pkg::wb_vrom_addr).
#define CHAR_ROM_SRAM_ADDRESS 0x68000

extern const uint8_t rom_chars_e800[0x800];
extern const uint8_t* const p_video_font_000;
extern const uint8_t* const p_video_font_400;

// Refresh the HDMI renderer's character ROM from FPGA SRAM.
void roms_refresh_char_rom(void);

// 1KB glyph table for the HDMI renderer.
const uint8_t* roms_get_char_rom(bool video_graphics);
const uint8_t* roms_get_ascii_char_rom(void);

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
