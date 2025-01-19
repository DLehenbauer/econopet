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

module address_decoding (
    input  logic                      sys_clock_i,

    input  logic                      cpu_be_i,
    input  logic                      cpu_wr_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,

    output logic                      ram_en_o,
    output logic                      sid_en_o,
    output logic                      magic_en_o,
    output logic                      pia1_en_o,
    output logic                      pia2_en_o,
    output logic                      via_en_o,
    output logic                      crtc_en_o,
    output logic                      io_en_o,
    output logic                      is_vram_o,
    output logic                      is_rom_o,

    output logic                     decoded_a15_o,
    output logic                     decoded_a16_o
);
    localparam RAM_EN_BIT       = 0,
               SID_EN_BIT       = 1,
               MAGIC_EN_BIT     = 2,
               PIA1_EN_BIT      = 3,
               PIA2_EN_BIT      = 4,
               VIA_EN_BIT       = 5,
               CRTC_EN_BIT      = 6,
               IO_EN_BIT        = 7,
               IS_ROM_BIT       = 8,
               IS_VRAM_BIT      = 9;

    localparam NUM_BITS         = 10;

    localparam RAM_EN_MASK       = NUM_BITS'(1'b1) << RAM_EN_BIT,
               SID_EN_MASK       = NUM_BITS'(1'b1) << SID_EN_BIT,
               MAGIC_EN_MASK     = NUM_BITS'(1'b1) << MAGIC_EN_BIT,
               PIA1_EN_MASK      = NUM_BITS'(1'b1) << PIA1_EN_BIT,
               PIA2_EN_MASK      = NUM_BITS'(1'b1) << PIA2_EN_BIT,
               VIA_EN_MASK       = NUM_BITS'(1'b1) << VIA_EN_BIT,
               CRTC_EN_MASK      = NUM_BITS'(1'b1) << CRTC_EN_BIT,
               IO_EN_MASK        = NUM_BITS'(1'b1) << IO_EN_BIT,
               IS_ROM_MASK       = NUM_BITS'(1'b1) << IS_ROM_BIT,
               IS_VRAM_MASK      = NUM_BITS'(1'b1) << IS_VRAM_BIT;

    localparam NONE  = NUM_BITS'('0),
               RAM   = RAM_EN_MASK,
               VRAM  = RAM_EN_MASK  | IS_VRAM_MASK,
               SID   = SID_EN_MASK,
               MAGIC = MAGIC_EN_MASK,
               ROM   = RAM_EN_MASK  | IS_ROM_MASK,
               PIA1  = PIA1_EN_MASK | IO_EN_MASK,
               PIA2  = PIA2_EN_MASK | IO_EN_MASK,
               VIA   = VIA_EN_MASK  | IO_EN_MASK,
               CRTC  = CRTC_EN_MASK;                // No IO_EN: CRTC implemented on FPGA

    logic [NUM_BITS-1:0] select = NUM_BITS'('hxxx);

    always_comb begin
        if (!cpu_be_i) begin
            select = NONE;
        end else begin
            priority casez (cpu_addr_i)
                // PET memory map
                CPU_ADDR_WIDTH'('b0???_????_????_????): select = RAM;    // RAM   : 0000-7FFF
                CPU_ADDR_WIDTH'('b1000_1111_????_????): select = SID;    // SID   : 8F00-8FFF
                CPU_ADDR_WIDTH'('b1000_????_????_????): select = VRAM;   // VRAM  : 8000-8FFF (overlaps with SID)
                CPU_ADDR_WIDTH'('b1110_1000_0000_????): select = MAGIC;  // MAGIC : E800-E80F
                CPU_ADDR_WIDTH'('b1110_1000_0001_????): select = PIA1;   // PIA1  : E810-E81F
                CPU_ADDR_WIDTH'('b1110_1000_001?_????): select = PIA2;   // PIA2  : E820-E83F
                CPU_ADDR_WIDTH'('b1110_1000_01??_????): select = VIA;    // VIA   : E840-E87F
                CPU_ADDR_WIDTH'('b1110_1000_1???_????): select = CRTC;   // CRTC  : E880-E8FF
                default:                                select = ROM;    // ROM   : 9000-E800, E900-FFFF
            endcase
        end
    end

    assign ram_en_o         = select[RAM_EN_BIT];
    assign is_rom_o         = select[IS_ROM_BIT];
    assign is_vram_o        = select[IS_VRAM_BIT];

    assign sid_en_o         = select[SID_EN_BIT];
    assign magic_en_o       = select[MAGIC_EN_BIT];
    assign io_en_o          = select[IO_EN_BIT];
    assign pia1_en_o        = select[PIA1_EN_BIT];
    assign pia2_en_o        = select[PIA2_EN_BIT];
    assign via_en_o         = select[VIA_EN_BIT];
    assign crtc_en_o        = select[CRTC_EN_BIT];

    assign decoded_a15_o    = cpu_addr_i[15];
    assign decoded_a16_o    = 1'b0;
endmodule
