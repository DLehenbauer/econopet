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

#include "driver.h"
#include "pet.h"

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

// Edit 4.0, 40 column, Graphics Keyboard, 50 Hz, CRTC
static const uint8_t __in_flash(".rom_edit_4_40_n_50Hz") rom_edit_4_40_n_50Hz[] = {
    #include "roms/edit-4-40-n-50Hz.901498-01.h"
};

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

const uint8_t* const p_video_font_000 = rom_chars_8800;
const uint8_t* const p_video_font_400 = rom_chars_8800 + 0x400;

void pet_init_edit_rom(bool is80Columns, bool isBusinessKeyboard, bool is50Hz) {
    if (isBusinessKeyboard) {
        spi_write(/* dest: */ 0xe000, /* pSrc: */ rom_edit_4_80_b_60Hz, sizeof(rom_edit_4_80_b_60Hz));
    } else {
        if (is80Columns) {
            spi_write(/* dest: */ 0xe000, /* pSrc: */ rom_edit_4_80_n_60Hz, sizeof(rom_edit_4_80_n_60Hz));
        } else {
            if (is50Hz) {
                spi_write(/* dest: */ 0xe000, /* pSrc: */ rom_edit_4_40_n_50Hz, sizeof(rom_edit_4_40_n_50Hz));
            } else {
                spi_write(/* dest: */ 0xe000, /* pSrc: */ rom_edit_4_40_n_60Hz, sizeof(rom_edit_4_40_n_60Hz));
            }
        }
    }
}

void pet_init_roms(bool is80Columns, bool isBusinessKeyboard, bool is50Hz) {
    spi_write(/* dest: */ 0x8800, /* pSrc: */ rom_chars_8800,  sizeof(rom_chars_8800));
    spi_write(/* dest: */ 0xb000, /* pSrc: */ rom_basic_b000,  sizeof(rom_basic_b000));
    spi_write(/* dest: */ 0xc000, /* pSrc: */ rom_basic_c000,  sizeof(rom_basic_c000));
    spi_write(/* dest: */ 0xd000, /* pSrc: */ rom_basic_d000,  sizeof(rom_basic_d000));
    pet_init_edit_rom(is80Columns, isBusinessKeyboard, is50Hz);
    spi_write(/* dest: */ 0xf000, /* pSrc: */ rom_kernal_f000, sizeof(rom_kernal_f000));
}
