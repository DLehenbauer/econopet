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

#include "../pch.h"

extern const uint8_t rom_chars_e800[0x800];
extern const uint8_t* const p_video_font_000;
extern const uint8_t* const p_video_font_400;

/**
 * Reason for starting the menu ROM. Each entry corresponds to a jump table
 * entry in the menu ROM at $FF00 (see rom/src/main.s).
 */
typedef enum {
    MENU_ROM_BOOT_NORMAL = 0,  // Jump table entry 0
    MENU_ROM_BOOT_ERROR  = 1,  // Jump table entry 1
} menu_rom_boot_reason_t;

void start_menu_rom(menu_rom_boot_reason_t reason);
