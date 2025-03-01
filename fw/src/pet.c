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
    printf("Requested ROM: %s-%s-%sHz\n",
        is80Columns ? "80" : "40",
        isBusinessKeyboard ? "b" : "n",
        is50Hz ? "50" : "60"
    );
   
    if (isBusinessKeyboard) {
        spi_write(/* dest: */ 0xe000, /* pSrc: */ rom_edit_4_80_b_60Hz, sizeof(rom_edit_4_80_b_60Hz));
        set_video(true);
        printf("Actual ROM: 80-b-60Hz\n");
    } else {
        if (is80Columns) {
            spi_write(/* dest: */ 0xe000, /* pSrc: */ rom_edit_4_80_n_60Hz, sizeof(rom_edit_4_80_n_60Hz));
            set_video(true);
            printf("Actual ROM: 80-n-60Hz\n");
        } else {
            if (is50Hz) {
                spi_write(/* dest: */ 0xe000, /* pSrc: */ rom_edit_4_40_n_50Hz, sizeof(rom_edit_4_40_n_50Hz));
                set_video(false);
                printf("Actual ROM: 40-n-50Hz\n");
            } else {
                spi_write(/* dest: */ 0xe000, /* pSrc: */ rom_edit_4_40_n_60Hz, sizeof(rom_edit_4_40_n_60Hz));
                set_video(false);
                printf("Actual ROM: 40-n-60Hz\n");
            }
        }
    }
}

void pet_reset() {
    // Per the W65C02S datasheet (sections 3.10 - 3.11):
    //
    //  * The CPU requires a minimum of 2 clock cycles to reset.  The CPU clock is 1 MHz (1 us period).
    //  * Deasserting RDY prevents the CPU from advancing it's state on negative PHI2 edges.
    // 
    // (See: https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf)

    // Out of paranoia, deassert CPU 'reset' to ensure the CPU observes a clean reset pulse.
    // (We set 'ready' to false to prevent the CPU from executing instructions.)
    set_cpu(/* ready: */ false, /* reset: */ false, /* nmi: */ false);
    sleep_us(4);
    
    // Assert CPU 'reset'.  Execution continues to be suspended by deasserting 'ready'.
    set_cpu(/* ready: */ false, /* reset: */ true, /* nmi: */ false);
    sleep_us(4);
    
    // Finally, deassert CPU 'reset' and assert 'ready' to allow the CPU to execute instructions.
    set_cpu(/* ready: */ true,  /* reset: */ false, /* nmi: */ false);
}

void pet_init_roms(bool is80Columns, bool isBusinessKeyboard, bool is50Hz) {
    // Ensure CPU is suspended while initializing ROMs.
    set_cpu(/* ready: */ false, /* reset: */ false, /* nmi: */ false);
    sleep_us(4);

    spi_write(/* dest: */ 0x8800, /* pSrc: */ rom_chars_8800,  sizeof(rom_chars_8800));
    spi_write(/* dest: */ 0xb000, /* pSrc: */ rom_basic_b000,  sizeof(rom_basic_b000));
    spi_write(/* dest: */ 0xc000, /* pSrc: */ rom_basic_c000,  sizeof(rom_basic_c000));
    spi_write(/* dest: */ 0xd000, /* pSrc: */ rom_basic_d000,  sizeof(rom_basic_d000));
    pet_init_edit_rom(is80Columns, isBusinessKeyboard, is50Hz);
    spi_write(/* dest: */ 0xf000, /* pSrc: */ rom_kernal_f000, sizeof(rom_kernal_f000));

    // Commodore's '8032.mem.prg', which is included with the 64K RAM expansion, tests
    // "I/O peek through" by reading the address $E800 and asserting the result is $E8:
    //
    //      .C:f974  AD 00 E8    LDA $E800
    //      .C:f977  C9 E8       CMP #$E8
    //      .C:f979  F0 07       BEQ $F982
    //
    // This is surprising as $E800 is unmapped.  After discussion with the Commodore PET/CBM
    // Enthusiasts FB group, our hypothesis is that a combination of capacitance, slow low-Z
    // transition, and/or hysteresis of the 74LS244 buffers holds the last data value read
    // on the bus long enough that the 6502 can reliably read it back if there are no other
    // drivers.
    //
    // In the case of 'LDA $E800', the last byte read would be the high byte of the address
    // in the LDA instruction.  The same seems to hold true for other unmapped regions.  To
    // simulate this quirk, we initialize the RAM backing these unmapped regions with the high
    // byte of the address.
    //
    // Note that this is likely an imperfect emulation of the PET as it returns the high byte
    // of the address being read as opposed to holding the last byte transferred on the bus.
    // This can be detected using indexed addressing modes:
    //
    //      ; Set up zeropage pointer to the base address ($9FFF)
    //      LDA #$FF
    //      STA tmp1
    //      LDA #$9F
    //      STA tmp1+1
    //      
    //      ; Initialize the Y register
    //      LDY #$0         ; Start indexing at offset 0
    //
    //      ; Read unmapped addresses $9FFF and $A000 using the same base address of $9FFF.
    //      ; In both cases, the last bus transfer should be the CPU fetching 'tmp1 + 1' ($9F).
    //
    //      LDA (tmp1),Y    ; Read $9FFF -> expect A=$9F (value fetched from 'tmp1 + 1')
    //      INY             ; Increment Y to read the next byte
    //      LDA (tmp1),Y    ; Read $A000 -> expect A=$9F (value fetched from 'tmp1 + 1')
    //
    // Our simulation will return $9F and A0 instead of the expect $9F for both reads.
    // (VICE 3.9 does the same.)

    // Skip 8800-8FFF because we currently use it for the character ROM.
    // for (uint32_t addr = 0x8000; addr < 0x8FFF; addr += 0x100) {
    //     spi_fill(addr, /* byte: */ addr >> 8, /* byteLength: */ 0x100);
    // }

    // 9000-EFFF
    for (uint32_t addr = 0x9000; addr < 0xAFFF; addr += 0x100) {
        spi_fill(addr, /* byte: */ addr >> 8, /* byteLength: */ 0x100);
    }

    // E800-E80F
    spi_fill(0xE800, /* byte: */ 0xE8, /* byteLength: **/ 0x10);

    // E900-FFFF
    for (uint32_t addr = 0xE900; addr < 0xF000; addr += 0x100) {
        spi_fill(addr, /* byte: */ addr >> 8, /* byteLength: */ 0x100);
    }

    pet_reset();
}

void pet_nmi() {
    // Out of paranoia, deassert CPU 'NMI' to ensure the CPU observes a clean pulse.
    // (We set 'ready' to false to prevent the CPU from executing instructions.)
    set_cpu(/* ready: */ false, /* reset: */ false, /* nmi: */ false);
    sleep_us(4);
    
    // Assert CPU 'nmi'.  Execution continues to be suspended by deasserting 'ready'.
    set_cpu(/* ready: */ false, /* reset: */ false, /* nmi: */ true);
    sleep_us(4);
    
    // Finally, deassert CPU 'NMI' and assert 'ready' to allow the CPU to execute instructions.
    set_cpu(/* ready: */ true,  /* reset: */ false, /* nmi: */ false);
}
