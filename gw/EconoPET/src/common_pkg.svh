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

`ifndef COMMON_PKG_SVH
`define COMMON_PKG_SVH

package common_pkg;
    //
    // Timing
    //

    // There are three places clock frequencies are defined, which must be kept in sync:
    //
    //   1.  Here
    //   2.  In the *.sdc
    //   3.  In the interface designer (*.peri.xml)
    //
    localparam real SYS_CLOCK_MHZ = 64;
    localparam real SPI_SCK_MHZ = 24;

    function real mhz_to_ns(input real freq_mhz);
        return 1000.0 / freq_mhz;
    endfunction

    function int ns_to_cycles(input int time_ns);
        return int'($ceil(time_ns / mhz_to_ns(SYS_CLOCK_MHZ)));
    endfunction

    // Calculates the required bit width to store the given value.
    function int bit_width(input int value);
        return $clog2(value + 1'b1);
    endfunction

    localparam int unsigned DATA_WIDTH      = 8;                                // 6502 and Wishbone data buses are 1 byte wide.
    localparam int unsigned BIT_INDEX_WIDTH = bit_width(DATA_WIDTH - 1'b1);     // Bits required to index into a byte (0-7).

    //
    // PET
    //

    // The PET keyboard matrix is 8 rows x 10 columns.
    localparam int unsigned KBD_ROW_COUNT = 8;      // 0-7 (A-F,H-J)
    localparam int unsigned KBD_COL_COUNT = 10;     // 0-9 (1-10)
    
    // PIA: Peripheral Interface Adapter
    // https://www.westerndesigncenter.com/wdc/documentation/w65c21.pdf
    // http://archive.6502.org/datasheets/rockwell_r6520_pia.pdf

    localparam int unsigned PIA_RS_WIDTH = 2;

    // PIA register addresses.
    localparam bit [   PIA_RS_WIDTH-1:0] PIA_PORTA = 0,         // DDRA/PIBA: Data Direction / Peripheral Interface Buffer for Port A
                                         PIA_CRA   = 1,         // CRA: Control Register A.  (Bit 2 selects 0=DDRA or 1=PIBA.)
                                         PIA_PORTB = 2,         // DDRB/PIBB: Data Direction / Peripheral Interface Buffer for Port B
                                         PIA_CRB   = 3;         // CRB: Control Register B.  (Bit 2 selects 0=DDRB or 1=PIBB.)

    // PIBA bit assignments.
    localparam bit [BIT_INDEX_WIDTH-1:0] PIA_PORTA_KEY_A = 0,   // PIA_PORTA[3:0]: Drives Key A-D, which selects the
                                         PIA_PORTA_KEY_D = 3;   // currently active column during a keyboard scan.

    // VIA: Versatile Interface Adapter
    // https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf

    localparam int unsigned VIA_RS_WIDTH = 4;

    localparam VIA_PORTB = 4'h0,    // IRB/ORB: Input/Output Register for Port B
               VIA_PORTA = 4'h1,    // IRA/ORA: Input/Output Register for Port A
               VIA_DDRB  = 4'h2,    // DDRB: Data Direction Register for Port B
               VIA_DDRA  = 4'h3,    // DDRA: Data Direction Register for Port A
               VIA_T1CL  = 4'h4,    // T1C-L: T1 Low-Order Latches / Counter
               VIA_T1CH  = 4'h5,    // T1C-H: T1 High-Order Counter 
               VIA_T1LL  = 4'h6,    // T1L-L: T1 Low-Order Latches 
               VIA_T1LH  = 4'h7,    // T1L-H: T1 High-Order Latches 
               VIA_T2CL  = 4'h8,    // T2C-L: T2 Low-Order Latches / Counter
               VIA_T2CH  = 4'h9,    // T2C-H: T2 High-Order Counter 
               VIA_SR    = 4'hA,    // SR: Shift Register 
               VIA_ACR   = 4'hB,    // ACR: Auxiliary Control Register 
               VIA_PCR   = 4'hC,    // PCR: Peripheral Control Register 
               VIA_IFR   = 4'hD,    // IFR: Interrupt Flag Register 
               VIA_IER   = 4'hE,    // IER: Interrupt Enable Register 
               VIA_ORA   = 4'hF;    // IRA/ORA: Same as Reg 1 except no "Handshake"

    // CRTC: CRT Controller
    // http://archive.6502.org/datasheets/rockwell_r6545-1_crtc.pdf

    localparam CRTC_R0_H_TOTAL            = 0,   // [7:0] Total displayed and non-displayed characters, minus one, per horizontal line.
                                                 //       The frequency of HSYNC is thus determined by this register.
                
               CRTC_R1_H_DISPLAYED        = 1,   // [7:0] Number of displayed characters per horizontal line.
                
               CRTC_R2_H_SYNC_POS         = 2,   // [7:0] Position of the HSYNC on the horizontal line, in terms of the character location number on the line.
                                                 //       The position of the HSYNC determines the left-to-right location of the displayed text on the video screen.
                                                 //       In this way, the side margins are adjusted.

               CRTC_R3_SYNC_WIDTH         = 3,   // [3:0] Width of HSYNC in character clock times (0 = HSYNC off)
                                                 // [7:4] Width of VSYNC in scan lines (0 = 16 scanlines)

               CRTC_R4_V_TOTAL            = 4,   // [6:0] Total number of character rows in a frame, minus one. This register, along with R5,
                                                 //       determines the overall frame rate, which should be close to the line frequency to
                                                 //       ensure flicker-free appearance. If the frame time is adjusted to be longer than the
                                                 //       period of the line frequency, then /RES may be used to provide absolute synchronism.

               CRTC_R5_V_ADJUST           = 5,   // [4:0] Number of additional scan lines needed to complete an entire frame scan and is intended
                                                 //       as a fine adjustment for the video frame time.

               CRTC_R6_V_DISPLAYED        = 6,   // [6:0] Number of displayed character rows in each frame. In this way, the vertical size of the
                                                 //       displayed text is determined.
            
               CRTC_R7_V_SYNC_POS         = 7,   // [6:0] Selects the character row time at which the VSYNC pulse is desired to occur and, thus,
                                                 //       is used to position the displayed text in the vertical direction.

               CRTC_R8_MODE_CONTREL       = 8,   // [7:0] Selects operating mode [Not implemented]
                                                 //
                                                 //       [0] Must be 0
                                                 //       [1] Not used
                                                 //       [2] RAD: Refresh RAM addressing Mode (0 = straight binary, 1 = row/col)
                                                 //       [3] Must be 0
                                                 //       [4] DES: Display Enable Skew (0 = no delay, 1 = delay Display Enable one char at a time)
                                                 //       [5] CSK: Cursor Skew (0 = no delay, 1 = delay Cursor one char at a time)
                                                 //       [6] Not used
                                                 //       [7] Not used

               CRTC_R9_MAX_SCAN_LINE      = 9,   // [4:0] Number of scan lines per character row, including spacing.

               CRTC_R10_CURSOR_START_LINE = 10,  // [6:0] Cursor blink mode and starting scan line [Not implemented]
                                                 //
                                                 //       [6:5] Cursor blink mode with respect to field rate.
                                                 //             (00 = on, 01 = off, 10 = 1/16 rate, 11 = 1/32 rate)
                                                 //
                                                 //       [4:0] Starting scan line

               CRTC_R11_CURSOR_END_LINE   = 11,  // [4:0] Ending scan line of cursor [Not implemented]

               CRTC_R12_START_ADDR_HI     = 12,  // [5:0] High 6 bits of 14 bit display address (starting address of screen_addr_o[13:8]).

               CRTC_R13_START_ADDR_LO     = 13,  // [7:0] Low 8 bits of 14 bit display address (starting address of screen_addr_o[7:0]).
               
               CRTC_R14_CURSOR_POS_HIGH   = 14,  // [5:0] 14-bit memory address (MA) of current cursor position [Not implemented]
               CRTC_R15_CURSOR_POS_LOW    = 15,  // [7:0]
               
               CRTC_R16_LIGHT_PEN_HIGH    = 16,  // [5:0] 14-bit video display address at which lightpen strobe occured [Not implemented]
               CRTC_R17_LIGHT_PEN_LOW     = 17,  // [7:0]

               CRTC_REG_COUNT             = CRTC_R17_LIGHT_PEN_LOW + 1'b1;

    // Bit width of the CRTC's internal address register.
    localparam int unsigned CRTC_ADDR_REG_WIDTH = bit_width(CRTC_REG_COUNT - 1'b1);

    //
    // Registers
    //

    localparam REG_IO_KBD  = 1'b0;
    localparam REG_IO_PIA1 = 3'b1_00;
    localparam REG_IO_PIA2 = 3'b1_01;

    localparam int unsigned REG_IO_KEY_MATRIX_START = {27'b0, REG_IO_KBD, 4'h0},
                            REG_IO_KEY_MATRIX_END   = REG_IO_KEY_MATRIX_START + KBD_COL_COUNT - 1,
                            REG_IO_PIA1_PORTA       = {27'b0, REG_IO_PIA1, PIA_PORTA},
                            REG_IO_PIA1_CRA         = {27'b0, REG_IO_PIA1, PIA_CRA},
                            REG_IO_PIA1_PORTB       = {27'b0, REG_IO_PIA1, PIA_PORTB},
                            REG_IO_PIA1_CRB         = {27'b0, REG_IO_PIA1, PIA_CRB},
                            REG_IO_PIA2_PORTA       = {27'b0, REG_IO_PIA2, PIA_PORTA},
                            REG_IO_PIA2_CRA         = {27'b0, REG_IO_PIA2, PIA_CRA},
                            REG_IO_PIA2_PORTB       = {27'b0, REG_IO_PIA2, PIA_PORTB},
                            REG_IO_PIA2_CRB         = {27'b0, REG_IO_PIA2, PIA_CRB},
                            REG_IO_VIA_PORTB        = REG_IO_PIA2_CRB + 1'b1,
                            IO_REG_COUNT            = REG_IO_VIA_PORTB + 1'b1;

    localparam int unsigned REG_CPU           = 0;
    localparam int unsigned REG_VIDEO         = 1;
    localparam int unsigned REG_COUNT         = 2;

    localparam int unsigned REG_CPU_READY_BIT    = 0;
    localparam int unsigned REG_CPU_RESET_BIT    = 1;

    localparam int unsigned REG_VIDEO_COL_80_BIT   = 0;
    localparam int unsigned REG_VIDEO_GRAPHICS_BIT = 1;  // VIA CA2 (0 = graphics, 1 = text)

    //
    // Bus
    //

    localparam int unsigned WB_ADDR_WIDTH   = 20;
    localparam int unsigned RAM_ADDR_WIDTH  = 17;
    localparam int unsigned VRAM_ADDR_WIDTH = 11;
    localparam int unsigned VROM_ADDR_WIDTH = 12;
    localparam int unsigned CPU_ADDR_WIDTH  = 16;
    localparam int unsigned REG_ADDR_WIDTH  = bit_width(REG_COUNT - 1'b1);
    localparam int unsigned KBD_ADDR_WIDTH  = bit_width(KBD_COL_COUNT - 1'b1);

    localparam WB_RAM_BASE  = 3'b000;
    localparam WB_REG_BASE  = 4'b0100;
    localparam WB_CRTC_BASE = 4'b0101;
    localparam WB_KBD_BASE  = 3'b011;
    localparam WB_VRAM_BASE = { WB_RAM_BASE, 6'b010000 };   // SRAM: $8000-87FF
    localparam WB_VROM_BASE = { WB_RAM_BASE, 6'b010001 };   // SRAM: $8800-8FFF

    // TODO: Move some of these address helpers to ../sim?
    function logic[WB_ADDR_WIDTH-1:0] wb_ram_addr(input logic[RAM_ADDR_WIDTH-1:0] address);
        return { WB_RAM_BASE, address };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_reg_addr(input logic[REG_ADDR_WIDTH-1:0] register);
        return { WB_REG_BASE, (WB_ADDR_WIDTH - REG_ADDR_WIDTH - $bits(WB_REG_BASE))'('x), register };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_crtc_addr(input logic[CRTC_ADDR_REG_WIDTH-1:0] register);
        return { WB_CRTC_BASE, (WB_ADDR_WIDTH - CRTC_ADDR_REG_WIDTH - $bits(WB_CRTC_BASE))'('x), register };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_kbd_addr(input logic[KBD_ADDR_WIDTH-1:0] register);
        return { WB_KBD_BASE, (WB_ADDR_WIDTH - KBD_ADDR_WIDTH - $bits(WB_KBD_BASE))'('x), register };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_vram_addr(input logic[VRAM_ADDR_WIDTH-1:0] address);
        return { WB_VRAM_BASE, address };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_vrom_addr(input logic[VROM_ADDR_WIDTH-1:0] address);
        // TODO: We currently only have 2KB of VROM mapped at $8800-$8FFF.
        //       Therefore, we ignore the CHR_OPTION passed as the msb of the address.
        //       (See 'CHR_OPTION' signal on sheets 8 and 10 of Universal Dynamic PET.)
        return { WB_VROM_BASE, address[VROM_ADDR_WIDTH-2:0] };
    endfunction
endpackage

`endif
