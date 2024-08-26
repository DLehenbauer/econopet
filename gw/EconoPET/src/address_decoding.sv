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


`include "./src/common_pkg.svh"

import common_pkg::*;

module address_decoding(
    input  logic [WB_ADDR_WIDTH-1:0] addr_i,
    output logic ram_en_o,
    output logic sid_en_o,
    output logic magic_en_o,
    output logic pia1_en_o,
    output logic pia2_en_o,
    output logic via_en_o,
    output logic crtc_en_o,
    output logic io_en_o,
    output logic is_mirrored_o,
    output logic is_readonly_o,
    output logic reg_en_o
);
    localparam RAM_EN_BIT       = 0,
               SID_EN_BIT       = 1,
               MAGIC_EN_BIT     = 2,
               PIA1_EN_BIT      = 3,
               PIA2_EN_BIT      = 4,
               VIA_EN_BIT       = 5,
               CRTC_EN_BIT      = 6,
               IO_EN_BIT        = 7,
               RAM_READONLY_BIT = 8,
               RAM_MIRRORED_BIT = 9,
               REG_EN_BIT       = 10;

    localparam NUM_BITS         = 11;

    localparam RAM_EN_MASK       = NUM_BITS'(1'b1) << RAM_EN_BIT,
               SID_EN_MASK       = NUM_BITS'(1'b1) << SID_EN_BIT,
               MAGIC_EN_MASK     = NUM_BITS'(1'b1) << MAGIC_EN_BIT,
               PIA1_EN_MASK      = NUM_BITS'(1'b1) << PIA1_EN_BIT,
               PIA2_EN_MASK      = NUM_BITS'(1'b1) << PIA2_EN_BIT,
               VIA_EN_MASK       = NUM_BITS'(1'b1) << VIA_EN_BIT,
               CRTC_EN_MASK      = NUM_BITS'(1'b1) << CRTC_EN_BIT,
               IO_EN_MASK        = NUM_BITS'(1'b1) << IO_EN_BIT,
               RAM_READONLY_MASK = NUM_BITS'(1'b1) << RAM_READONLY_BIT,
               RAM_MIRRORED_MASK = NUM_BITS'(1'b1) << RAM_MIRRORED_BIT,
               REG_EN_MASK       = NUM_BITS'(1'b1) << REG_EN_BIT;

    localparam NONE  = NUM_BITS'('0),
               RAM   = RAM_EN_MASK,
               VRAM  = RAM_EN_MASK  | RAM_MIRRORED_MASK,
               SID   = SID_EN_MASK,
               MAGIC = MAGIC_EN_MASK,
               ROM   = RAM_EN_MASK  | RAM_READONLY_MASK,
               PIA1  = PIA1_EN_MASK | IO_EN_MASK,
               PIA2  = PIA2_EN_MASK | IO_EN_MASK,
               VIA   = VIA_EN_MASK  | IO_EN_MASK,
               CRTC  = CRTC_EN_MASK,                // No IO_EN: CRTC implemented on FPGA
               REG   = REG_EN_MASK;

    logic [NUM_BITS-1:0] select = NUM_BITS'('hxxx);

    always_comb begin
        priority casez (addr_i)
            // Register file
            { WB_REG_PREFIX, RAM_ADDR_WIDTH'('b?_????_????_????_????) }: select = REG;    // REG   : Register File

            // Flat access to SRAM
            { WB_RAM_PREFIX, RAM_ADDR_WIDTH'('b?_????_????_????_????) }: select = RAM;    // RAM   : 00000-1FFFF

            // PET memory map
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b0_0???_????_????_????) }: select = RAM;    // RAM   : 0000-7FFF
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b0_1000_1111_????_????) }: select = SID;    // SID   : 8F00-8FFF
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b0_1000_????_????_????) }: select = VRAM;   // VRAM  : 8000-8F00
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b0_1110_1000_0000_????) }: select = MAGIC;  // MAGIC : E800-E80F
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b0_1110_1000_0001_????) }: select = PIA1;   // PIA1  : E810-E81F
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b0_1110_1000_001?_????) }: select = PIA2;   // PIA2  : E820-E83F
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b0_1110_1000_01??_????) }: select = VIA;    // VIA   : E840-E87F
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b0_1110_1000_1???_????) }: select = CRTC;   // CRTC  : E880-E8FF
            { WB_CPU_PREFIX, RAM_ADDR_WIDTH'('b?_????_????_????_????) }: select = ROM;    // ROM   : 9000-E800, E900-FFFF

            default: select = NONE;
        endcase
    end

    assign ram_en_o       = select[RAM_EN_BIT];
    assign is_readonly_o  = select[RAM_READONLY_BIT];
    assign is_mirrored_o  = select[RAM_MIRRORED_BIT];

    assign sid_en_o       = select[SID_EN_BIT];
    assign magic_en_o     = select[MAGIC_EN_BIT];
    assign io_en_o        = select[IO_EN_BIT];
    assign pia1_en_o      = select[PIA1_EN_BIT];
    assign pia2_en_o      = select[PIA2_EN_BIT];
    assign via_en_o       = select[VIA_EN_BIT];
    assign crtc_en_o      = select[CRTC_EN_BIT];
    assign reg_en_o       = select[REG_EN_BIT];
endmodule
