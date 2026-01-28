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

    localparam int unsigned DATA_WIDTH      = 8;                    // 6502 and Wishbone data buses are 1 byte wide.
    localparam int unsigned BIT_INDEX_WIDTH = $clog2(DATA_WIDTH);   // Bits required to index into a byte (0-7).

    //
    // PET
    //

    // The PET keyboard matrix is 8 rows x 10 columns.
    localparam int unsigned KBD_ROW_COUNT = 8;      // 0-7 (A-F,H-J)
    localparam int unsigned KBD_COL_COUNT = 10;     // 0-9 (1-10)
    localparam int unsigned KBD_COL_WIDTH = $clog2(KBD_COL_COUNT);
    
    // PIA: Peripheral Interface Adapter
    // https://www.westerndesigncenter.com/wdc/documentation/w65c21.pdf
    // http://archive.6502.org/datasheets/rockwell_r6520_pia.pdf

    localparam int unsigned PIA_RS_WIDTH = 2;

    // PIA register addresses.
    localparam bit [   PIA_RS_WIDTH-1:0] PIA_PORTA = 0,     // DDRA/PIBA: Data Direction / Peripheral Interface Buffer for Port A
                                         PIA_CRA   = 1,     // CRA: Control Register A.  (Bit 2 selects 0=DDRA or 1=PIBA.)
                                         PIA_PORTB = 2,     // DDRB/PIBB: Data Direction / Peripheral Interface Buffer for Port B
                                         PIA_CRB   = 3;     // CRB: Control Register B.  (Bit 2 selects 0=DDRB or 1=PIBB.)

    // PIA1 Port A bit assignments
    localparam bit [BIT_INDEX_WIDTH-1:0] PIA1_PORTA_KEY_A_OUT       = 0, // Key A..D selects column (0-9) for keyboard scan
                                         PIA1_PORTA_KEY_D_OUT       = 3,
                                         PIA1_PORTA_CASS1_SWITCH_IN = 4, // Cassette #1 switch (0 = Closed, 1 = Open)
                                         PIA1_PORTA_CASS2_SWITCH_IN = 5, // Cassette #2 switch (0 = Closed, 1 = Open)
                                         PIA1_PORTA_EOI_IN          = 6, // IEEE-488: (0 = last byte from device, 1 = device has more data)
                                         PIA1_PORTA_DIAG_OUT        = 7; // Diagnostic sense (0 = TIM, 1 = BASIC)

    localparam bit [BIT_INDEX_WIDTH-1:0] PIA1_CRA_EOI_OUT           = 3; // IEEE-488: (0 = last byte from PET, 1 = PET has more data)

    // PIA2 Port A is DIO1-8 (In)

    localparam bit [BIT_INDEX_WIDTH-1:0] PIA2_CRA_NDAC_OUT          = 3, // IEEE-488: (0 = PET has not accepted data, 1 = PET has accepted data)
                                         PIA2_CRA_ATN_IN            = 7; // IEEE-488: (0 = Device wants to send command, 1 = Idle)

    // PIA2 Port B is DIO1-8 (Out)

    localparam bit [BIT_INDEX_WIDTH-1:0] PIA2_CRB_DAV_OUT           = 3, // IEEE-488: (0 = DIO1-8 is valid, 1 = DIO1-8 is not valid)
                                         PIA2_CRB_SRQ_IN            = 7; // IEEE-488: (0 = Device requesting service, 1 = No service requested)

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

    // VIA Port B bit assignments
    localparam bit [BIT_INDEX_WIDTH-1:0] VIA_PORTB_NDAC_IN         = 0,    // IEEE-488: (0 = Device has not accepted data, 1 = Device has accepted data)
                                         VIA_PORTB_NRFD_OUT        = 1,    // IEEE-488: (0 = PET not ready for data, 1 = PET ready for data)
                                         VIA_PORTB_ATN_OUT         = 2,    // IEEE-488: (0 = PET wants to send command, 1 = Idle)
                                         VIA_PORTB_CASS_WRITE_OUT  = 3,    // Cassette Write Signal
                                         VIA_PORTB_CASS2_MOTOR_OUT = 4,    // Cassette #2 Motor On (0 = On, 1 = Off)
                                         VIA_PORTB_VERT_RETRACE_IN = 5,    // Vertical Retrace Detect
                                         VIA_PORTB_NRFD_IN         = 6,    // IEEE-488: (0 = Device not ready for data, 1 = Device ready for data)
                                         VIA_PORTB_DAV_IN          = 7;    // IEEE-488: (0 = DIO1-8 is valid, 1 = DIO1-8 is not valid)

    // CRTC (MOS6545) registers
    // https://ia600203.us.archive.org/5/items/crtc6545/crtc6545.pdf
    // http://archive.6502.org/datasheets/rockwell_r6545-1_crtc.pdf

    localparam CRTC_R0_H_TOTAL            = 0,  // [7:0] Total displayed and non-displayed characters, minus one, per horizontal line.
                                                //       The frequency of HSYNC is thus determined by this register.
                
               CRTC_R1_H_DISPLAYED        = 1,  // [7:0] Number of displayed characters per horizontal line.
                
               CRTC_R2_H_SYNC_POS         = 2,  // [7:0] Position of the HSYNC on the horizontal line, in terms of the character location number on the line.
                                                //       The position of the HSYNC determines the left-to-right location of the displayed text on the video screen.
                                                //       In this way, the side margins are adjusted.

               CRTC_R3_SYNC_WIDTH         = 3,  // [3:0] Width of HSYNC in character clock times (0 = HSYNC off)
                                                // [7:4] Width of VSYNC in scan lines (0 = 16 scanlines)

               CRTC_R4_V_TOTAL            = 4,  // [6:0] Total number of character rows in a frame, minus one. This register, along with R5,
                                                //       determines the overall frame rate, which should be close to the line frequency to
                                                //       ensure flicker-free appearance. If the frame time is adjusted to be longer than the
                                                //       period of the line frequency, then /RES may be used to provide absolute synchronism.

               CRTC_R5_V_ADJUST           = 5,  // [4:0] Number of additional scan lines needed to complete an entire frame scan and is intended
                                                //       as a fine adjustment for the video frame time.

               CRTC_R6_V_DISPLAYED        = 6,  // [6:0] Number of displayed character rows in each frame. In this way, the vertical size of the
                                                //       displayed text is determined.
            
               CRTC_R7_V_SYNC_POS         = 7,  // [6:0] Selects the character row time at which the VSYNC pulse is desired to occur and, thus,
                                                //       is used to position the displayed text in the vertical direction.

               CRTC_R8_MODE_CONTROL       = 8,  // [7:0] Selects operating mode [Not implemented]
                                                //
                                                //       [0] Must be 0
                                                //       [1] Not used
                                                //       [2] RAD: Refresh RAM addressing Mode (0 = straight binary, 1 = row/col)
                                                //       [3] Must be 0
                                                //       [4] DES: Display Enable Skew (0 = no delay, 1 = delay Display Enable one char at a time)
                                                //       [5] CSK: Cursor Skew (0 = no delay, 1 = delay Cursor one char at a time)
                                                //       [6] Not used
                                                //       [7] Not used

               CRTC_R9_MAX_SCAN_LINE      = 9,  // [4:0] Number of scan lines per character row, minus one, including spacing.
                                                //
                                                //       Graphics Mode: 7 → 8 scan lines/row (full character)
                                                //           Text Mode: 9 → 10 scan lines/row (full character + 2 blank lines)

               CRTC_R10_CURSOR_START_LINE = 10, // [6:0] Cursor blink mode and starting scan line [Not implemented]
                                                //
                                                //       [6:5] Cursor blink mode with respect to field rate.
                                                //             (00 = on, 01 = off, 10 = 1/16 rate, 11 = 1/32 rate)
                                                //
                                                //       [4:0] Starting scan line

               CRTC_R11_CURSOR_END_LINE   = 11, // [4:0] Ending scan line of cursor [Not implemented]

               CRTC_R12_START_ADDR_HI     = 12, // [5:0] High 6 bits of 14 bit display address (starting address of screen_addr_o[13:8]).
               CRTC_R13_START_ADDR_LO     = 13, // [7:0] Low 8 bits of 14 bit display address (starting address of screen_addr_o[7:0]).
                                                //
                                                //       Note that on the PET, bits MA[13:12] do not address video RAM and instead are
                                                //       used for special functions:
                                                //
                                                //         [5] TA13 selects an alternative character rom (0 = normal, 1 = international)
                                                //         [4] TA12 inverts the video signal (0 = inverted, 1 = normal)
                                                //
                                                //       [Motorola CRTC only] Allows readback of R12-13 [Not implemented]

               
               CRTC_R14_CURSOR_POS_HIGH   = 14, // [5:0] 14-bit memory address (MA) of current cursor position [Not implemented]
               CRTC_R15_CURSOR_POS_LOW    = 15, // [7:0] (All variants can read R14-R15 - [Not implemented])
               
               CRTC_R16_LIGHT_PEN_HIGH    = 16, // [5:0] 14-bit video display address at which lightpen strobe occured [Not implemented]
               CRTC_R17_LIGHT_PEN_LOW     = 17, // [7:0] (All variants can read R16-R17 - [Not implemented])

               CRTC_R31_DUMMY_REG         = 31, // [Rockwell R6545 only] Used with UR (Update Ready) status bit [Not implemented]

               CRTC_REG_COUNT             = CRTC_R31_DUMMY_REG + 1'b1;

    // Bit width of the CRTC's internal address register.
    localparam int unsigned CRTC_ADDR_REG_WIDTH = $clog2(CRTC_REG_COUNT);

    // CRTC Status Register Bits (read from address $E880 when RS=0)
    localparam CRTC_STATUS_UPDATE_READY_BIT = 7,  // [Rockwell R6545 only] Update Ready (0 = R31 read/written, 1 = update strobe occurred) [Not implemented]
               CRTC_STATUS_LPEN_BIT         = 6,  // LPEN Register Full (0 = R16/R17 read, 1 = LPEN strobe occurred) [Not implemented]
               CRTC_STATUS_VBLANK_BIT       = 5;  // Vertical Blanking  (0 = not in vblank, 1 = in vblank)

    // SID: Sound Interface Device
    // https://archive.org/details/mos-6581-sid-data-sheet/page/n3/mode/2up
    // http://www.cbmhardware.de/show.php?r=14&id=71/PETSID
    
    localparam SID_R0_VOICE1_FREQ_LO            = 0,
               SID_R1_VOICE1_FREQ_HI            = 1,
               SID_R2_VOICE1_PW_LO              = 2,
               SID_R3_VOICE1_PW_HI              = 3,
               SID_R4_VOICE1_CONTROL            = 4,
               SID_R5_VOICE1_ATTACK_DECAY       = 5,
               SID_R6_VOICE1_SUSTAIN_RELEASE    = 6,
               SID_R7_VOICE2_FREQ_LO            = 7,
               SID_R8_VOICE2_FREQ_HI            = 8,
               SID_R9_VOICE2_PW_LO              = 9,
               SID_R10_VOICE2_PW_HI             = 10,
               SID_R11_VOICE2_CONTROL           = 11,
               SID_R12_VOICE2_ATTACK_DECAY      = 12,
               SID_R13_VOICE2_SUSTAIN_RELEASE   = 13,
               SID_R14_VOICE3_FREQ_LO           = 14,
               SID_R15_VOICE3_FREQ_HI           = 15,
               SID_R16_VOICE3_PW_LO             = 16,
               SID_R17_VOICE3_PW_HI             = 17,
               SID_R18_VOICE3_CONTROL           = 18,
               SID_R19_VOICE3_ATTACK_DECAY      = 19,
               SID_R20_VOICE3_SUSTAIN_RELEASE   = 20,
               SID_R21_FC_LO                    = 21,
               SID_R22_FC_HI                    = 22,
               SID_R23_RES_FILT                 = 23,
               SID_R24_MODE_VOL                 = 24,
               SID_R25_POTX                     = 25,
               SID_R26_POTY                     = 26,
               SID_R27_OSC3                     = 27,
               SID_R28_ENV3                     = 28,
               SID_REG_COUNT                    = SID_R28_ENV3 + 1'b1;
    
    localparam int unsigned SID_ADDR_REG_WIDTH = $clog2(SID_REG_COUNT);

    // 64K RAM Expansion

    // Memory control register at $FFF0.
    // (See http://6502.org/users/andre/petindex/8x96.html)
    localparam bit [BIT_INDEX_WIDTH-1:0] MEM_CTL_ENABLE           = 7,    // Enable expansion memory (1 = enabled, 0 = disabled)
                                         MEM_CTL_IO_PEEK          = 6,    // I/O peek-through at $E800-$EFFF (1 = enabled, 0 = disabled)
                                         MEM_CTL_SCREEN_PEEK      = 5,    // Screen peek-through at $8000-$8FFF (1 = enabled, 0 = disabled)
                                         MEM_CTL_RESERVED         = 4,    // Reserved
                                         MEM_CTL_SELECT_HI        = 3,    // Selects 16KB page at $C000-$FFFF (1 = block3, 0 = block2)
                                         MEM_CTL_SELECT_LO        = 2,    // Selects 16KB page at $8000-$BFFF (1 = block1, 0 = block0)
                                         MEM_CTL_WRITE_PROTECT_HI = 1,    // Write protects $C000-$FFFF excluding peek-through (1 = read only, 0 = read/write)
                                         MEM_CTL_WRITE_PROTECT_LO = 0;    // Write protects $8000-$BFFF excluding peek-through (1 = read only, 0 = read/write)

    //
    // Registers
    //

    localparam REG_IO_VIA  = 2'b00;
    localparam REG_IO_PIA1 = 4'b0100;
    localparam REG_IO_PIA2 = 4'b1001;

    localparam bit [5:0] REG_IO_VIA_PORTB        = {REG_IO_VIA, VIA_PORTB},
                         REG_IO_VIA_PORTA        = {REG_IO_VIA, VIA_PORTA},
                         REG_IO_VIA_DDRB         = {REG_IO_VIA, VIA_DDRB},
                         REG_IO_VIA_DDRA         = {REG_IO_VIA, VIA_DDRA},
                         REG_IO_VIA_T1CL         = {REG_IO_VIA, VIA_T1CL},
                         REG_IO_VIA_T1CH         = {REG_IO_VIA, VIA_T1CH},
                         REG_IO_VIA_T1LL         = {REG_IO_VIA, VIA_T1LL},
                         REG_IO_VIA_T1LH         = {REG_IO_VIA, VIA_T1LH},
                         REG_IO_VIA_T2CL         = {REG_IO_VIA, VIA_T2CL},
                         REG_IO_VIA_T2CH         = {REG_IO_VIA, VIA_T2CH},
                         REG_IO_VIA_SR           = {REG_IO_VIA, VIA_SR},
                         REG_IO_VIA_ACR          = {REG_IO_VIA, VIA_ACR},
                         REG_IO_VIA_PCR          = {REG_IO_VIA, VIA_PCR},
                         REG_IO_VIA_IFR          = {REG_IO_VIA, VIA_IFR},
                         REG_IO_VIA_IER          = {REG_IO_VIA, VIA_IER},
                         REG_IO_VIA_ORA          = {REG_IO_VIA, VIA_ORA},
                         REG_IO_PIA1_PORTA       = {REG_IO_PIA1, PIA_PORTA},
                         REG_IO_PIA1_CRA         = {REG_IO_PIA1, PIA_CRA},
                         REG_IO_PIA1_PORTB       = {REG_IO_PIA1, PIA_PORTB},
                         REG_IO_PIA1_CRB         = {REG_IO_PIA1, PIA_CRB},
                         REG_IO_PIA2_PORTA       = {REG_IO_PIA2, PIA_PORTA},
                         REG_IO_PIA2_CRA         = {REG_IO_PIA2, PIA_CRA},
                         REG_IO_PIA2_PORTB       = {REG_IO_PIA2, PIA_PORTB},
                         REG_IO_PIA2_CRB         = {REG_IO_PIA2, PIA_CRB},
                         IO_REG_COUNT            = REG_IO_PIA2_CRB + 1'b1;

    localparam int unsigned IO_REG_ADDR_WIDTH = $clog2(IO_REG_COUNT);

    //
    // Register file
    //

    // Register 0: Status (Read-only)
    localparam int unsigned REG_STATUS              = 0;
    localparam int unsigned REG_STATUS_GRAPHICS_BIT = 0;    // VIA CA2 (0 = graphics, 1 = text)
    localparam int unsigned REG_STATUS_CRT_BIT      = 1;    // Diagonal CRT size (0 = 12", 1 = 9")
    localparam int unsigned REG_STATUS_KEYBOARD_BIT = 2;    // Keyboard Type (0 = Business, 1 = Graphics)

    // Register 1: CPU control
    localparam int unsigned REG_CPU                 = 1;
    localparam int unsigned REG_CPU_READY_BIT       = 0;
    localparam int unsigned REG_CPU_RESET_BIT       = 1;
    localparam int unsigned REG_CPU_NMI_BIT         = 2;
    
    // Register 2: Video Control
    localparam int unsigned REG_VIDEO                   = 2;
    localparam int unsigned REG_VIDEO_COL_80_BIT        = 0;
    localparam int unsigned REG_VIDEO_RAM_MASK_LO_BIT   = 1;    // video_ram_mask[10]
    localparam int unsigned REG_VIDEO_RAM_MASK_HI_BIT   = 2;    // video_ram_mask[11]

    localparam int unsigned REG_COUNT                   = REG_VIDEO + 1'b1;

    //
    // Bus
    //

    localparam int unsigned WB_ADDR_WIDTH   = 20;
    localparam int unsigned RAM_ADDR_WIDTH  = 17;
    localparam int unsigned VRAM_ADDR_WIDTH = 11;
    localparam int unsigned VROM_ADDR_WIDTH = 12;
    localparam int unsigned CPU_ADDR_WIDTH  = 16;
    localparam int unsigned REG_ADDR_WIDTH  = $clog2(REG_COUNT);

    // TODO: Consider arranging our address space such that the MCU can read VRAM,
    //       keyboard status, and the status register in a single SPI transaction.

    // Note: WB_RAM_BASE reserves an extra bit for the RAM address.  This double
    //       maps the RAM address space as follows:
    //
    //         $00000-$1ffff -> $00000-$1ffff
    //         $20000-$3ffff -> $00000-$1ffff
    //
    //       This allows SPI / wishbone reads and writes to "wrap-around".  This
    //       is particularily useful for 'read_next' at 0x1ffff, which otherwise
    //       would stall indefinately because attempting to read $20000 would
    //       deselect the wishbone RAM peripheral.
    localparam WB_RAM_BASE  = 2'b00;
    localparam WB_REG_BASE  = 4'b0100;
    localparam WB_CRTC_BASE = 4'b0101;
    localparam WB_KBD_BASE  = 5'b01100;
    localparam WB_BRAM_BASE = 5'b01101;
    localparam WB_VRAM_BASE = { WB_RAM_BASE, 7'b0010000 };   // SRAM: $8000-87FF
    localparam WB_VROM_BASE = { WB_RAM_BASE, 7'b0011101 };   // SRAM: $E800-EFFF

    // BRAM address width for character ROM (4KB = 2^12 bytes)
    localparam int unsigned BRAM_ADDR_WIDTH = 12;

    // TODO: Move some of these address helpers to ../sim?
    function logic[WB_ADDR_WIDTH-1:0] wb_ram_addr(input logic[RAM_ADDR_WIDTH-1:0] address);
        return { WB_RAM_BASE, 1'b0, address };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_reg_addr(input logic[REG_ADDR_WIDTH-1:0] register);
        return { WB_REG_BASE, (WB_ADDR_WIDTH - REG_ADDR_WIDTH - $bits(WB_REG_BASE))'('0), register };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_crtc_addr(input logic[CRTC_ADDR_REG_WIDTH-1:0] register);
        return { WB_CRTC_BASE, (WB_ADDR_WIDTH - CRTC_ADDR_REG_WIDTH - $bits(WB_CRTC_BASE))'('0), register };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_kbd_addr(input logic[KBD_COL_WIDTH-1:0] register);
        return { WB_KBD_BASE, (WB_ADDR_WIDTH - KBD_COL_WIDTH - $bits(WB_KBD_BASE))'('0), register };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_bram_addr(input logic[BRAM_ADDR_WIDTH-1:0] address);
        return { WB_BRAM_BASE, (WB_ADDR_WIDTH - BRAM_ADDR_WIDTH - $bits(WB_BRAM_BASE))'('0), address };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_vram_addr(input logic[VRAM_ADDR_WIDTH-1:0] address);
        return { WB_VRAM_BASE, address };
    endfunction

    function logic[WB_ADDR_WIDTH-1:0] wb_vrom_addr(input logic[VROM_ADDR_WIDTH-1:0] address);
        // Character ROM is now stored in BRAM at $68000-$68FFF (4KB).
        // The full 12-bit address is used, supporting both character sets.
        // (See 'CHR_OPTION' signal on sheets 8 and 10 of Universal Dynamic PET.)
        return wb_bram_addr(address);
    endfunction
endpackage
