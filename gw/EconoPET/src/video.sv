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
    // Wishbone B4 controller
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

    input  logic cpu_reset_i,
    input  logic wr_strobe_i,
    input  logic cclk_en_i,
    input  logic crtc_cs_i,                       // CRTC selected for data transfer (driven by address decoding)
    input  logic crtc_we_i,                       // Direction of data transfer (0 = writing to CRTC, 1 = reading from CRTC)
    input  logic crtc_rs_i,                       // Register select (0 = write address/read status, 1 = read addressed register)
    input  logic [DATA_WIDTH-1:0] crtc_data_i,    // Transfer data written from CPU to CRTC when CS asserted and /RW is low
    output logic [DATA_WIDTH-1:0] crtc_data_o,    // Transfer data read by CPU from CRTC when CS asserted and /RW is high
    output logic crtc_data_oe,                    // CRTC drives data lines (CPU is reading from CRTC)

    input  logic col_80_mode_i,                   // (0 = 40 col, 1 = 80 col)
    input  logic graphic_i,                       // Selects character set via A10 of VROM. (0 = upper/gfx, 1 = lower/upper)

    output logic h_sync_o,                        // Horizontal sync
    output logic v_sync_o                         // Vertical sync
);
    initial begin
        wb_we_o     = '0;
        wb_cycle_o  = '0;
        wb_strobe_o = '0;
    end

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
        .cclk_en_i(cclk_en_i),        // Enables character clock (always 1 MHz)
        .h_sync_o(h_sync_o),          // Horizontal sync
        .v_sync_o(v_sync_o),          // Vertical sync
        .de_o(de),                    // Display enable
        .ma_o(ma),                    // Refresh RAM address lines
        .ra_o(ra)                     // Raster address lines
    );

    localparam EVEN_RAM = 0,
               EVEN_ROM = 1,
               ODD_RAM  = 2,
               ODD_ROM  = 3;

    logic [WB_ADDR_WIDTH-1:0] addrs [3:0];
    always_comb begin
        addrs[EVEN_RAM] = common_pkg::wb_vram_addr(col_80_mode_i
            ? { ma[9:0], 1'b0 }     // 80 column mode
            : { 1'b0, ma[9:0] });   // 40 column mode
        addrs[EVEN_ROM] = common_pkg::wb_vrom_addr({ graphic_i, data[EVEN_RAM][6:0], ra[2:0] });
        addrs[ODD_RAM]  = common_pkg::wb_vram_addr({ ma[9:0], 1'b1 });
        addrs[ODD_ROM]  = common_pkg::wb_vrom_addr({ graphic_i, data[ODD_RAM][6:0], ra[2:0] });
    end

    logic [DATA_WIDTH-1:0] data [3:0];

    localparam WB_IDLE = 0,
               WB_AWAIT_ACK = 1;

    logic [1:0] fetch_stage = EVEN_RAM;
    logic [0:0] wb_state    = WB_IDLE;

    always_ff @(posedge wb_clock_i) begin
        wb_strobe_o <= '0;
        
        case (wb_state)
            WB_IDLE: begin
                if (!wb_stall_i) begin
                    wb_addr_o   <= addrs[fetch_stage];
                    wb_cycle_o  <= 1'b1;
                    wb_strobe_o <= 1'b1;
                    wb_state    <= WB_AWAIT_ACK;
                end
            end

            WB_AWAIT_ACK: begin
                if (wb_ack_i) begin
                    data[fetch_stage] <= wb_data_i;
                    fetch_stage       <= fetch_stage + 1'b1;
                    wb_state          <= WB_IDLE;
                end
            end
        endcase
    end
endmodule