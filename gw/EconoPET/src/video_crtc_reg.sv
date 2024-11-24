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

module video_crtc_reg (
    // Wishbone B4 peripheral to read current CRTC register values.
    // (See: https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic wb_clock_i,                      // FPGA System clock
    input  logic [WB_ADDR_WIDTH-1:0] wb_addr_i,   // Address of pending read/write (valid when 'cycle_o' asserted)
    output logic [   DATA_WIDTH-1:0] wb_data_o,   // Data received from MCU to write (valid when 'cycle_o' asserted)
    input  logic wb_we_i,                         // Direction of transaction (0 = read , 1 = write)
    input  logic wb_cycle_i,                      // Bus cycle is active
    input  logic wb_strobe_i,                     // New transaction requested (address, data, and control signals are valid)
    output logic wb_stall_o,                      // Peripheral is not ready to accept the request
    output logic wb_ack_o,                        // Indicates success termination of cycle (data_o is valid)
    
    input  logic clk_en_i,                  // Gates 'sys_clock_i' to advance CRTC state
    input  logic [DATA_WIDTH-1:0] data_i,   // Transfer data written from CPU to CRTC when CS asserted and /RW is low
    input  logic cs_i,                      // CRTC selected for data transfer (driven by address decoding)
    input  logic we_i,                      // Direction of date transfers (0 = reading from CRTC, 1 = writing to CRTC)
    input  logic rs_i,                      // Register select (0 = write address/read status, 1 = read addressed register)

    output logic [ 7:0] r0_h_total_o,
    output logic [ 7:0] r1_h_displayed_o,
    output logic [ 7:0] r2_h_sync_pos_o,
    output logic [ 3:0] r3_h_sync_width_o,
    output logic [ 4:0] r3_v_sync_width_o,
    output logic [ 6:0] r4_v_total_o,
    output logic [ 4:0] r5_v_adjust_o,
    output logic [ 6:0] r6_v_displayed_o,
    output logic [ 6:0] r7_v_sync_pos_o,
    output logic [ 4:0] r9_max_scan_line_o,
    output logic [13:0] r1213_start_addr_o
);
    // Storage for R0..17 is rounded up to the nearest power of 2.
    localparam integer unsigned CRTC_REG_COUNT = (1 << CRTC_ADDR_REG_WIDTH);

    logic [CRTC_ADDR_REG_WIDTH-1:0] ar = '0;        // Address register used to select R0..17
    logic [DATA_WIDTH-1:0] r[CRTC_REG_COUNT-1:0];   // Storage for R0..17

    initial begin
`ifdef DEFAULT_15KHZ
        // Power-on state emulates a non-CRTC PET (15kHz, 60Hz)
        // The appropriate Editor ROM will reconfigure the CRTC for later models.
        //
        // Note that the parent module handles inverting the video signal according
        // to 'config_crtc_i'.  Therefore, we continue to set TA12 to '1'.
        r[CRTC_R0_H_TOTAL]           = 8'd63;
        r[CRTC_R1_H_DISPLAYED]       = 8'd40;
        r[CRTC_R2_H_SYNC_POS]        = 8'd48;
        r[CRTC_R3_SYNC_WIDTH]        = 8'h51;
        r[CRTC_R4_V_TOTAL]           = { 1'd0, 7'd32 };
        r[CRTC_R5_V_ADJUST]          = { 3'd0, 5'd05 };
        r[CRTC_R6_V_DISPLAYED]       = { 1'd0, 7'd25 };
        r[CRTC_R7_V_SYNC_POS]        = { 1'd0, 7'd28 };
        r[CRTC_R9_MAX_SCAN_LINE]     = { 3'd0, 5'd07 };
        r[CRTC_R12_START_ADDR_HI]    = 8'h10;    // TA12 inverts video (1 = normal, 0 = inverted)
        r[CRTC_R13_START_ADDR_LO]    = 8'h00;
`else
        // 8032 Power-On State
        r[CRTC_R0_H_TOTAL]           = 8'h31;
        r[CRTC_R1_H_DISPLAYED]       = 8'h28;
        r[CRTC_R2_H_SYNC_POS]        = 8'h29;
        r[CRTC_R3_SYNC_WIDTH]        = 8'h0f;
        r[CRTC_R4_V_TOTAL]           = 8'h20;
        r[CRTC_R5_V_ADJUST]          = 8'h03;
        r[CRTC_R6_V_DISPLAYED]       = 8'h19;
        r[CRTC_R7_V_SYNC_POS]        = 8'h1d;
        r[CRTC_R9_MAX_SCAN_LINE]     = 8'h09;
        r[CRTC_R12_START_ADDR_HI]    = 8'h10;    // TA12 inverts video (1 = normal, 0 = inverted)
        r[CRTC_R13_START_ADDR_LO]    = 8'h00;
`endif
    end

    // TODO: Need CRTC addr base

    logic wb_select;
    wb_decode #(WB_CRTC_BASE) wb_decode (
        .wb_addr_i(wb_addr_i),
        .selected_o(wb_select)
    );

    // Wishbone peripheral always completes in a single cycle.
    assign wb_stall_o = 1'b0;
    wire [CRTC_ADDR_REG_WIDTH-1:0] crtc_addr = wb_addr_i[CRTC_ADDR_REG_WIDTH-1:0];

    always_ff @(posedge wb_clock_i) begin
        wb_ack_o <= '0;
        if (wb_select && wb_cycle_i && wb_strobe_i) begin
            wb_data_o <= r[crtc_addr];
            
            // TODO: Support writes?
            // if (wb_we_i) begin
            //     r[crtc_addr] <= data_i;
            // end
            
            wb_ack_o <= 1'b1;
        end else if (clk_en_i && cs_i && we_i) begin
            if (rs_i == '0) ar <= data_i[4:0];  // RS = 0: Write to address register (select R0..17)
            else r[ar] <= data_i;               // RS = 1: Write to currently addressed register (set R0..17)
        end
    end

    assign r0_h_total_o       = r[CRTC_R0_H_TOTAL];
    assign r1_h_displayed_o   = r[CRTC_R1_H_DISPLAYED];
    assign r2_h_sync_pos_o    = r[CRTC_R2_H_SYNC_POS];
    assign r3_h_sync_width_o  = r[CRTC_R3_SYNC_WIDTH][3:0];
    assign r3_v_sync_width_o  = r[CRTC_R3_SYNC_WIDTH][7:4] == 0             // Per Datasheet, when bits 4-7 are all
                                    ? 5'h10                                 // "0", VSYNC will be 16 scan lines wide.
                                    : { 1'b0, r[CRTC_R3_SYNC_WIDTH][7:4] };
    assign r4_v_total_o       = r[CRTC_R4_V_TOTAL][6:0];
    assign r5_v_adjust_o      = r[CRTC_R5_V_ADJUST][4:0];
    assign r6_v_displayed_o   = r[CRTC_R6_V_DISPLAYED][6:0];
    assign r7_v_sync_pos_o    = r[CRTC_R7_V_SYNC_POS][6:0];
    assign r9_max_scan_line_o = r[CRTC_R9_MAX_SCAN_LINE][4:0];
    assign r1213_start_addr_o = { r[CRTC_R12_START_ADDR_HI][5:0], r[CRTC_R13_START_ADDR_LO] };
endmodule
