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

module video (
    // Video timing signals
    input  logic clk1_en_i,                       // 1 MHz character clock enable
    input  logic clk8_en_i,                       // 8 MHz pixel clock for 40 column mode
    input  logic clk16_en_i,                      // 16 MHz pixel clock for 80 column mode

    // Wishbone B4 controller to fetch character and pixel data from VRAM and VROM.
    // (See: https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic wb_clock_i,                      // Bus clock
    output logic [WB_ADDR_WIDTH-1:0] wb_addr_o,   // Address of pending read/write (valid when 'cycle_o' asserted)
    output logic [   DATA_WIDTH-1:0] wb_data_o,   // Data received from MCU to write (valid when 'cycle_o' asserted)
    input  logic [   DATA_WIDTH-1:0] wb_data_i,   // Data to transmit to MCU (captured on 'wb_clock_i' when 'wb_ack_i' asserted)
    output logic wb_we_o,                         // Direction of bus transfer (0 = reading, 1 = writing)
    output logic wb_cycle_o,                      // Requests a bus cycle from the arbiter
    output logic wb_strobe_o,                     // Signals next request ('addr_o', 'data_o', and 'wb_we_o' are valid).
    input  logic wb_stall_i,                      // Signals that peripheral is not ready to accept request
    input  logic wb_ack_i,                        // Signals termination of cycle ('data_i' valid)

    // CRTC interface to CPU bus
    input  logic cpu_reset_i,
    input  logic wr_strobe_i,
    input  logic crtc_cs_i,                       // CRTC selected for data transfer (driven by address decoding)
    input  logic crtc_we_i,                       // Direction of data transfer (0 = writing to CRTC, 1 = reading from CRTC)
    input  logic crtc_rs_i,                       // Register select (0 = write address/read status, 1 = read addressed register)
    input  logic [DATA_WIDTH-1:0] crtc_data_i,    // Transfer data written from CPU to CRTC when CS asserted and /RW is low
    output logic [DATA_WIDTH-1:0] crtc_data_o,    // Transfer data read by CPU from CRTC when CS asserted and /RW is high
    output logic crtc_data_oe,                    // CRTC drives data lines (CPU is reading from CRTC)

    input  logic col_80_mode_i,                   // (0 = 40 col, 1 = 80 col)
    input  logic graphic_i,                       // Selects character set via A10 of VROM. (0 = upper/gfx, 1 = lower/upper)

    output logic h_sync_o,                        // Horizontal sync
    output logic v_sync_o,                        // Vertical sync
    output logic video_o                          // Video output
);
    initial begin
        wb_we_o     = '0;
        wb_cycle_o  = '0;
        wb_strobe_o = '0;
    end

    //
    // CRTC
    //

    logic        de;    // Display enable
    logic [13:0] ma;
    logic [ 4:0] ra;

    video_crtc video_crtc (
        .reset_i(cpu_reset_i),
        .sys_clock_i(wb_clock_i),     // System clock
        .wr_strobe_i(wr_strobe_i),    // Write strobe from CPU
        .cs_i(crtc_cs_i),             // CRTC selected for data transfer (driven by address decoding)
        .rw_ni(!crtc_we_i),           // Direction of date transfers (0 = writing to CRTC, 1 = reading from CRTC)
        .rs_i(crtc_rs_i),             // Register select (0 = write address/read status, 1 = read addressed register)
        .data_i(crtc_data_i),         // Transfer data written from CPU to CRTC when CS asserted and /RW is low
        .data_o(crtc_data_o),         // Transfer data read by CPU from CRTC when CS asserted and /RW is high
        .data_oe(crtc_data_oe),       // Asserted when CPU is reading from CRTC
        .cclk_en_i(clk1_en_i),        // Character clock enable (always 1 MHz)
        .h_sync_o(h_sync_o),          // Horizontal sync
        .v_sync_o(v_sync_o),          // Vertical sync
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

    wire [WB_ADDR_WIDTH-1:0] even_ram_addr = common_pkg::wb_vram_addr(col_80_mode_i
            ? { ma[9:0], 1'b0 }     // 80 column mode
            : { 1'b0, ma[9:0] });   // 40 column mode
    wire [WB_ADDR_WIDTH-1:0] even_rom_addr = common_pkg::wb_vrom_addr({ crtc_chr_option, graphic_i, data[EVEN_RAM][6:0], ra[2:0] });
    wire [WB_ADDR_WIDTH-1:0] odd_ram_addr  = common_pkg::wb_vram_addr({ ma[9:0], 1'b1 });
    wire [WB_ADDR_WIDTH-1:0] odd_rom_addr  = common_pkg::wb_vrom_addr({ crtc_chr_option, graphic_i, data[ODD_RAM][6:0], ra[2:0] });
    logic [WB_ADDR_WIDTH-1:0] addrs [3:0];

    always_comb begin
        addrs[EVEN_RAM] = even_ram_addr;
        addrs[EVEN_ROM] = even_rom_addr;
        addrs[ODD_RAM]  =  odd_ram_addr;
        addrs[ODD_ROM]  =  odd_rom_addr;
    end

    logic [DATA_WIDTH-1:0] data [3:0];

    localparam WB_IDLE = 0,
               WB_AWAIT_ACK = 1;

    logic [1:0] fetch_stage = EVEN_RAM;
    logic [0:0] wb_state    = WB_IDLE;

    always_ff @(posedge wb_clock_i) begin
        case (wb_state)
            WB_IDLE: begin
                wb_strobe_o <= 1'b1;
                wb_cycle_o <= 1'b1;

                if (!wb_stall_i) begin
                    wb_addr_o   <= addrs[fetch_stage];
                    wb_state    <= WB_AWAIT_ACK;
                end
            end

            WB_AWAIT_ACK: begin
                wb_strobe_o <= 1'b0;

                if (wb_ack_i) begin
                    data[fetch_stage] <= wb_data_i;
                    fetch_stage       <= fetch_stage + 1'b1;
                    wb_state          <= WB_IDLE;
                end
            end
        endcase
    end

    wire [7:0] even_char = data[EVEN_RAM];
    wire [7:0] even_rom = data[EVEN_ROM];
    wire [7:0] odd_char = data[ODD_RAM];
    wire [7:0] odd_rom = data[ODD_ROM];
    wire [15:0] pixels = { even_rom, odd_rom };

    // Scanlines exceeding the 8 pixel high character ROM should be blanked.
    // (See 'NO_ROW' signal on sheets 8 and 10 of Universal Dynamic PET.)
    wire no_row = ra[3] || ra[4];

    //
    // Dotgen
    //

    wire pixel_clk_en = col_80_mode_i
        ? clk16_en_i
        : clk8_en_i;

    video_dotgen video_dotgen (
        .sys_clock_i(wb_clock_i),
        .pixel_clk_en_i(pixel_clk_en),
        .cclk_en_i(clk1_en_i),
        .pixels_i(pixels),
        .reverse_i({ even_char[7], odd_char[7] }),
        .display_en_i(de && !no_row),
        .video_o(video_o)
    );
endmodule
