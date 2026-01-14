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

import common_pkg::*;

module video (
    // Video timing signals
    input  logic clk8_en_i,                       // 8 MHz pixel clock for 40 column mode
    input  logic clk16_en_i,                      // 16 MHz pixel clock for 80 column mode

    // Wishbone B4 controller to fetch character and pixel data from VRAM and VROM.
    // (See: https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic wb_clock_i,                       // Bus clock
    output logic [WB_ADDR_WIDTH-1:0] wbc_addr_o,   // Address of pending read/write (valid when 'cycle_o' asserted)
    input  logic [   DATA_WIDTH-1:0] wbc_data_i,   // Data received from RAM (captured on 'wb_clock_i' when 'wbc_ack_i' asserted)
    output logic wbc_we_o,                         // Direction of bus transfer (0 = reading, 1 = writing)
    output logic wbc_cycle_o,                      // Requests a bus cycle from the arbiter
    output logic wbc_strobe_o,                     // Signals next request ('addr_o', 'data_o', and 'wbc_we_o' are valid).
    input  logic wbc_stall_i,                      // Signals that peripheral is not ready to accept request
    input  logic wbc_ack_i,                        // Signals termination of cycle ('data_i' valid)

    // Wishbone B4 peripheral to read/write current CRTC register values.
    // (See: https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic [WB_ADDR_WIDTH-1:0] wbp_addr_i,   // Address of pending read/write (valid when 'cycle_o' asserted)
    input  logic [   DATA_WIDTH-1:0] wbp_data_i,   // Data received from MCU to write (valid when 'cycle_o' asserted)
    output logic [   DATA_WIDTH-1:0] wbp_data_o,   // Data to transmit to MCU (valid when 'ack_o' asserted)
    input  logic wbp_we_i,                         // Direction of transaction (0 = read , 1 = write)
    input  logic wbp_cycle_i,                      // Bus cycle is active
    input  logic wbp_strobe_i,                     // New transaction requested (address, data, and control signals are valid)
    output logic wbp_stall_o,                      // Peripheral is not ready to accept the request
    output logic wbp_ack_o,                        // Indicates success termination of cycle (data_o is valid)
    input  logic wbp_sel_i,                        // Asserted when selected by 'wbp_addr_i'

    // CRTC interface to CPU bus
    input  logic cpu_reset_i,
    input  logic crtc_clk_en_i,
    input  logic crtc_cs_i,                       // CRTC selected for data transfer (driven by address decoding)
    input  logic crtc_we_i,                       // Direction of data transfer (0 = writing to CRTC, 1 = reading from CRTC)
    input  logic crtc_rs_i,                       // Register select (0 = write address/read status, 1 = read addressed register)
    input  logic [DATA_WIDTH-1:0] crtc_data_i,    // Transfer data written from CPU to CRTC when CS asserted and /RW is low
    output logic [DATA_WIDTH-1:0] crtc_data_o,    // Transfer data read by CPU from CRTC when CS asserted and /RW is high
    output logic crtc_data_oe,                    // CRTC drives data lines (CPU is reading from CRTC)

    // DotGen
    input  logic load_sr1_i,                      // 1 MHz clock user to load SR of dot generator
    input  logic load_sr2_i,                      // 2 MHz clock used to load SR of dot generator
    input  logic config_crt_i,                    // Select VDU (0 = 12"/CRTC, 1 = 9"/non-CRTC)
    input  logic col_80_mode_i,                   // (0 = 40 col, 1 = 80 col)
    input  logic graphic_i,                       // Selects character set via A10 of VROM. (0 = upper/gfx, 1 = lower/upper)
    output logic h_sync_o,                        // Horizontal sync
    output logic v_sync_o,                        // Vertical sync
    output logic video_o                          // Video output
);
    initial begin
        wbc_we_o     = '0;
        wbc_cycle_o  = '0;
        wbc_strobe_o = '0;
    end

    //
    // CRTC
    //

    logic        de;    // Display enable
    logic [13:0] ma;
    logic [ 4:0] ra;
    logic        crtc_h_sync;
    logic        crtc_v_sync;

    video_crtc video_crtc (
        .wb_clock_i(wb_clock_i),
        .wbp_addr_i(wbp_addr_i),
        .wbp_data_i(wbp_data_i),
        .wbp_data_o(wbp_data_o),
        .wbp_we_i(wbp_we_i),
        .wbp_cycle_i(wbp_cycle_i),
        .wbp_strobe_i(wbp_strobe_i),
        .wbp_stall_o(wbp_stall_o),
        .wbp_ack_o(wbp_ack_o),
        .wbp_sel_i(wbp_sel_i),

        .reset_i(cpu_reset_i),
        .clk_en_i(crtc_clk_en_i),     // 1 MHz clock enable for 'sys_clock_i'
        .cs_i(crtc_cs_i),             // CRTC selected for data transfer (driven by address decoding)
        .we_i(crtc_we_i),             // Direction of date transfers (0 = reading from CRTC, 1 = writing to CRTC)
        .rs_i(crtc_rs_i),             // Register select (0 = write address/read status, 1 = read addressed register)
        .data_i(crtc_data_i),         // Transfer data written from CPU to CRTC when CS asserted and /RW is low
        .data_o(crtc_data_o),         // Transfer data read by CPU from CRTC when CS asserted and /RW is high
        .data_oe(crtc_data_oe),       // Asserted when CPU is reading from CRTC
        .config_crt_i(config_crt_i),  // Select VDU (0 = 12"/CRTC, 1 = 9"/non-CRTC)
        .h_sync_o(crtc_h_sync),       // Horizontal sync (active high)
        .v_sync_o(crtc_v_sync),       // Vertical sync (active high)
        .de_o(de),                    // Display enable
        .ma_o(ma),                    // Refresh RAM address lines
        .ra_o(ra)                     // Raster address lines
    );

    wire crtc_invert     = ma[12];   // TA12 inverts the video signal (0 = inverted, 1 = normal)
    wire crtc_chr_option = ma[13];   // TA13 selects an alternative character rom (0 = normal, 1 = international)

    //
    // Fetch
    //

    localparam EVEN_RAM = 0,
               EVEN_ROM = 1,
               ODD_RAM  = 2,
               ODD_ROM  = 3;

    wire  [WB_ADDR_WIDTH-1:0] even_ram_addr = common_pkg::wb_vram_addr(col_80_mode_i
        ? { ma[9:0], 1'b0 }     // 80 column mode
        : { 1'b0, ma[9:0] });   // 40 column mode
    wire  [WB_ADDR_WIDTH-1:0] even_rom_addr = common_pkg::wb_vrom_addr({ crtc_chr_option, graphic_i, data[EVEN_RAM][6:0], ra[2:0] });
    wire  [WB_ADDR_WIDTH-1:0] odd_ram_addr  = common_pkg::wb_vram_addr({ ma[9:0], 1'b1 });
    wire  [WB_ADDR_WIDTH-1:0] odd_rom_addr  = common_pkg::wb_vrom_addr({ crtc_chr_option, graphic_i, data[ODD_RAM][6:0], ra[2:0] });
    logic [WB_ADDR_WIDTH-1:0] addrs [3:0];

    always_comb begin
        addrs[EVEN_RAM] = even_ram_addr;
        addrs[EVEN_ROM] = even_rom_addr;
        addrs[ODD_RAM]  =  odd_ram_addr;
        addrs[ODD_ROM]  =  odd_rom_addr;
    end

    logic [DATA_WIDTH-1:0] data [3:0];

    localparam WB_IDLE = 0,
               WB_REQ  = 1,
               WB_WAIT = 2;

    always_comb begin
        wbc_addr_o = addrs[fetch_stage];
    end

    logic [1:0] fetch_stage = EVEN_RAM;
    logic [1:0] wbc_state    = WB_IDLE;

    always_ff @(posedge wb_clock_i) begin
        case (wbc_state)
            WB_IDLE: begin
                wbc_strobe_o <= 0;
                wbc_cycle_o  <= 0;

                if (crtc_clk_en_i) begin
                    wbc_cycle_o  <= 1;
                    wbc_strobe_o <= 1;
                    wbc_state    <= WB_REQ;
                end
            end

            WB_REQ: begin
                if (!wbc_stall_i) begin
                    wbc_strobe_o <= 0;
                end

                if (wbc_ack_i) begin
                    data[fetch_stage] <= wbc_data_i;
                    fetch_stage       <= fetch_stage + 1'b1;
                    wbc_strobe_o       <= 1;
                    
                    if (fetch_stage == ODD_ROM) begin
                        wbc_cycle_o  <= 0;
                        wbc_strobe_o <= 0;
                        wbc_state    <= WB_IDLE;
                    end
                end
            end
        endcase
    end

    wire [ 7:0] even_char = data[EVEN_RAM];
    wire [ 7:0]  even_rom = data[EVEN_ROM];
    wire [ 7:0]  odd_char = data[ODD_RAM];
    wire [ 7:0]   odd_rom = data[ODD_ROM];

    //
    // Dotgen
    //

    wire pixel_clk_en = col_80_mode_i
        ? clk16_en_i
        : clk8_en_i;

    wire load_sr = col_80_mode_i
        ? load_sr2_i
        : load_sr1_i;

    logic dotgen_video;
    logic dotgen_en = '0;

    // Scan lines exceeding the 8 pixel high character ROM should be blanked.
    // (See 'NO_ROW' signal on sheets 8 and 10 of Universal Dynamic PET.)
    wire no_row = ra[3] || ra[4];

    logic       odd = 1'b1;
    logic [7:0] pixels;
    logic       reverse;

    always @(posedge wb_clock_i) begin
        // Latch pixel data and reverse bit on load_sr (1 or 2 MHz).
        if (load_sr) begin
            pixels  <= odd ? odd_rom : even_rom;
            reverse <= odd ? odd_char[7] : even_char[7];
        end

        // However, always toggle even/odd at 2 MHz.  In 40 column mode,
        // the even characters will be ignored.
        if (load_sr2_i) begin
            odd <= ~odd;
        end
    end

    logic dotgen_de = '0;

    always @(posedge wb_clock_i) begin
        if (load_sr1_i) begin
            dotgen_de <= de && !no_row;
        end
    end

    video_dotgen video_dotgen (
        .sys_clock_i(wb_clock_i),
        .pixel_clk_en_i(pixel_clk_en),
        .load_sr_i(load_sr),
        .pixels_i(pixels),
        .reverse_i(reverse),
        .display_en_i(dotgen_de),
        .video_o(dotgen_video)
    );

    // The 9" and 12" CRTs have different polarity requirements for video and h_sync signals.
    // We adjust the outputs based on the 'config_crt' input (0 = 12", 1 = 9").
    //
    //          9" CRT    12" CRT
    //  HSync: Active-H   Active-L
    //  VSync: Active-L   Active-L
    //  Video: Active-L   Active-H

    // Adjust signal delay to match measurements from a 8032 60Hz.
    // H-Sync asserted 300ns before the end of the visible video line.
    delay #(
        .DELAY_CYCLES(7),
        .INITIAL_VALUE(1'b1)
    ) h_delay (
        .clock_i(wb_clock_i),
        .reset_i(cpu_reset_i),
        .data_i(!crtc_h_sync ^ config_crt_i),   // Adjust polarity based on CRT type
        .data_o(h_sync_o)
    );

    // Adjust signal delay to match measurements from a 8032 60Hz.
    // V-Sync asserted 2.10866ms after the end of the last visible line.
    delay #(
        .DELAY_CYCLES(4),
        .INITIAL_VALUE(1'b0)
    ) v_delay (
        .clock_i(wb_clock_i),
        .reset_i(cpu_reset_i),
        .data_i(!crtc_v_sync),                  // Vertical sync is always active low
        .data_o(v_sync_o)
    );

    always_ff @(posedge wb_clock_i) begin
        video_o <= dotgen_video ^ config_crt_i; // Adjust polarity based on CRT type
    end
endmodule
