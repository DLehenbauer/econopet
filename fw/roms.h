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

#include "pch.h"

static const uint8_t __in_flash(".rom_chars_8800") rom_chars_8800[] = {
    #include "roms/characters-2.901447-10.h"
};

static const uint8_t __in_flash(".rom_basic_b000") rom_basic_b000[] = {
    #include "roms/basic-4-b000.901465-23.h"
};

static const uint8_t __in_flash(".rom_basic_c000") rom_basic_c000[] = {
    #include "roms/basic-4-c000.901465-20.h"
};

static const uint8_t __in_flash(".rom_basic_d000") rom_basic_d000[] = {
    #include "roms/basic-4-d000.901465-21.h"
};

//
// Edit Rom ($E000)
//

// Edit 4.0, 40 column, Graphics Keyboard, 60 Hz, CRTC
static const uint8_t __in_flash(".rom_edit_4_40_n_60Hz") rom_edit_4_40_n_60Hz[] = {
    #include "roms/edit-4-40-n-60Hz.901499-01.h"
};

// Edit 4.0, 80 column, Graphics Keyboard, 60 Hz, CRTC
static const uint8_t __in_flash(".rom_edit_4_80_n_60Hz") rom_edit_4_80_n_60Hz[] = {
    #include "roms/edit-4-80-n-60Hz.901474-03-hack.h"
};

// Edit 4.0, 80 column, Business Keyboard, 60 Hz, CRTC
static const uint8_t __in_flash(".rom_edit_4_80_b_60Hz") rom_edit_4_80_b_60Hz[] = {
    #include "roms/edit-4-80-b-60Hz.901474-03.h"
};

//
// Kernal Rom ($F000)
//

static const uint8_t __in_flash(".rom_kernal_f000") rom_kernal_f000[] = {
    #include "roms/kernal-4.901465-22.h"
};

static const uint8_t* const p_video_font_000 = rom_chars_8800;
static const uint8_t* const p_video_font_400 = rom_chars_8800 + 0x400;