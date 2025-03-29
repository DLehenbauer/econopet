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

module memory_control (
    input  logic                      reset_i,

    input  logic                      sys_clock_i,
    input  logic                      cpu_be_i,
    input  logic                      cpu_wr_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,

    output logic bank_en_o,
    output logic bank_a15_o,
    output logic bank_ro_o
);
    // RAM expansion is disabled at power on.
    logic [DATA_WIDTH-1:0] mem_ctl = 8'b0xxx_xxxx;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) begin
            mem_ctl <= 8'b0xxx_xxxx;
        end else if (cpu_wr_strobe_i && cpu_addr_i == 16'hFFF0) begin
            mem_ctl <= cpu_data_i;
        end
    end

    logic io_peek;      // Asserted when IO peek-through enabled and address is $8000-$8FFF.
    logic screen_peek;  // Asserted when screen peek-through enabled and address is $E800-$EFFF.

    always_comb begin
        io_peek     = '0;
        screen_peek = '0;
        
        priority casez (cpu_addr_i)
            CPU_ADDR_WIDTH'('b1000_????_????_????): begin   // $8000-$8FFF: Screen peek-through
                screen_peek = mem_ctl[MEM_CTL_SCREEN_PEEK];
            end
            CPU_ADDR_WIDTH'('b1110_1???_????_????): begin   // $E810-$EFFF: IO peek-through
                io_peek = mem_ctl[MEM_CTL_IO_PEEK];
            end
            default: ;                                      // No peek-through
        endcase
    end

    wire mem_enabled = mem_ctl[MEM_CTL_ENABLE];

    always_comb begin
        bank_en_o  = '0;
        bank_a15_o = 'x;  // Unused when bank_en is '0
        bank_ro_o  = 'x;

        if (mem_enabled) begin
            unique casez (cpu_addr_i)
                CPU_ADDR_WIDTH'('b10??_????_????_????): begin   // $8000-$BFFF: Lower bank (0/1)
                    bank_en_o  = !screen_peek;
                    bank_a15_o = mem_ctl[MEM_CTL_SELECT_LO];
                    bank_ro_o  = mem_ctl[MEM_CTL_WRITE_PROTECT_LO];
                end
                CPU_ADDR_WIDTH'('b11??_????_????_????): begin   // $C000-$FFFF: Upper bank (2/3)
                    bank_en_o  = !io_peek;
                    bank_a15_o = mem_ctl[MEM_CTL_SELECT_HI];
                    bank_ro_o  = mem_ctl[MEM_CTL_WRITE_PROTECT_HI];
                end
                default: ;                                      // No bank
            endcase
        end
    end
endmodule

module address_decoding (
    input  logic                      reset_i,
    input  logic                      sys_clock_i,

    input  logic                      cpu_be_i,
    input  logic                      cpu_wr_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,

    output logic                      ram_en_o,
    output logic                      sid_en_o,
    output logic                      pia1_en_o,
    output logic                      pia2_en_o,
    output logic                      via_en_o,
    output logic                      crtc_en_o,
    output logic                      io_en_o,
    output logic                      is_vram_o,
    output logic                      is_readonly_o,

    output logic                      decoded_a15_o,
    output logic                      decoded_a16_o
);
    logic bank_en;
    logic bank_a15;
    logic bank_ro;

    memory_control memory_control (
        .reset_i(reset_i),
        .sys_clock_i(sys_clock_i),
        .cpu_be_i(cpu_be_i),
        .cpu_wr_strobe_i(cpu_wr_strobe_i),
        .cpu_addr_i(cpu_addr_i),
        .cpu_data_i(cpu_data_i),

        .bank_en_o(bank_en),
        .bank_a15_o(bank_a15),
        .bank_ro_o(bank_ro)
    );

    localparam RAM_EN_BIT       = 0,
               SID_EN_BIT       = 1,
               PIA1_EN_BIT      = 2,
               PIA2_EN_BIT      = 3,
               VIA_EN_BIT       = 4,
               CRTC_EN_BIT      = 5,
               IO_EN_BIT        = 6,
               IS_READONLY_BIT  = 7,
               IS_VRAM_BIT      = 8;

    localparam NUM_BITS         = 9;

    localparam RAM_EN_MASK       = NUM_BITS'(1'b1) << RAM_EN_BIT,
               SID_EN_MASK       = NUM_BITS'(1'b1) << SID_EN_BIT,
               PIA1_EN_MASK      = NUM_BITS'(1'b1) << PIA1_EN_BIT,
               PIA2_EN_MASK      = NUM_BITS'(1'b1) << PIA2_EN_BIT,
               VIA_EN_MASK       = NUM_BITS'(1'b1) << VIA_EN_BIT,
               CRTC_EN_MASK      = NUM_BITS'(1'b1) << CRTC_EN_BIT,
               IO_EN_MASK        = NUM_BITS'(1'b1) << IO_EN_BIT,
               IS_READONLY_MASK  = NUM_BITS'(1'b1) << IS_READONLY_BIT,
               IS_VRAM_MASK      = NUM_BITS'(1'b1) << IS_VRAM_BIT;

    localparam NONE  = NUM_BITS'('0),
               RAM   = RAM_EN_MASK,
               VRAM  = RAM_EN_MASK  | IS_VRAM_MASK,
               SID   = SID_EN_MASK,                 // No IO_EN: SID implemented on FPGA
               ROM   = RAM_EN_MASK  | IS_READONLY_MASK,
               PIA1  = PIA1_EN_MASK | IO_EN_MASK,
               PIA2  = PIA2_EN_MASK | IO_EN_MASK,
               VIA   = VIA_EN_MASK  | IO_EN_MASK,
               CRTC  = CRTC_EN_MASK;                // No IO_EN: CRTC implemented on FPGA

    logic [NUM_BITS-1:0] select = NUM_BITS'('hxxx);

    initial begin
        select = NONE;
    end

    always_ff @(posedge sys_clock_i) begin
        if (!cpu_be_i) begin
            select <= NONE;
        end else begin
            if (bank_en) begin
                select <= bank_ro
                    ? ROM
                    : RAM;
            end else begin
                priority casez (cpu_addr_i)
                    // PET memory map
                    CPU_ADDR_WIDTH'('b0???_????_????_????): select <= RAM;    // RAM  : 0000-7FFF
                    CPU_ADDR_WIDTH'('b1000_1111_????_????): select <= SID;    // SID  : 8F00-8FFF (takes precedence over VRAM)
                    // verilator lint_off CASEOVERLAP
                    CPU_ADDR_WIDTH'('b1000_????_????_????): select <= VRAM;   // VRAM : 8000-8FFF (intentionally overlaps with SID)
                    // verilator lint_on CASEOVERLAP
                    CPU_ADDR_WIDTH'('b1110_1000_0001_????): select <= PIA1;   // PIA1 : E810-E81F
                    CPU_ADDR_WIDTH'('b1110_1000_001?_????): select <= PIA2;   // PIA2 : E820-E83F
                    CPU_ADDR_WIDTH'('b1110_1000_01??_????): select <= VIA;    // VIA  : E840-E87F
                    CPU_ADDR_WIDTH'('b1110_1000_1???_????): select <= CRTC;   // CRTC : E880-E8FF
                    default:                                select <= ROM;    // ROM  : 9000-E80F, E900-FFFF
                endcase
            end
        end
    end

    assign ram_en_o         = select[RAM_EN_BIT];
    assign is_readonly_o    = select[IS_READONLY_BIT];
    assign is_vram_o        = select[IS_VRAM_BIT];

    assign sid_en_o         = select[SID_EN_BIT];
    assign io_en_o          = select[IO_EN_BIT];
    assign pia1_en_o        = select[PIA1_EN_BIT];
    assign pia2_en_o        = select[PIA2_EN_BIT];
    assign via_en_o         = select[VIA_EN_BIT];
    assign crtc_en_o        = select[CRTC_EN_BIT];

    assign decoded_a15_o    = bank_en ? bank_a15 : cpu_addr_i[15];
    assign decoded_a16_o    = bank_en;
endmodule
