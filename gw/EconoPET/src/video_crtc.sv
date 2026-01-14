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

module video_crtc(
    // Wishbone B4 peripheral to read/write current CRTC register values.
    // (See: https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic wb_clock_i,
    input  logic [WB_ADDR_WIDTH-1:0] wbp_addr_i,
    input  logic [   DATA_WIDTH-1:0] wbp_data_i,
    output logic [   DATA_WIDTH-1:0] wbp_data_o,
    input  logic wbp_we_i,
    input  logic wbp_cycle_i,
    input  logic wbp_strobe_i,
    output logic wbp_stall_o,
    output logic wbp_ack_o,
    
    input  logic reset_i,
    input  logic clk_en_i,                  // Gates 'wb_clock_i' to advance CRTC state
    input  logic cs_i,                      // CRTC selected for data transfer (driven by address decoding)
    input  logic we_i,                      // Direction of date transfers (0 = reading from CRTC, 1 = writing to CRTC)

    input  logic rs_i,                      // Register select (0 = write address/read status, 1 = read addressed register)

    input  logic [DATA_WIDTH-1:0] data_i,   // Transfer data written from CPU to CRTC when CS asserted and /RW is low
    output logic [DATA_WIDTH-1:0] data_o,   // Transfer data read by CPU from CRTC when CS asserted and /RW is high
    output logic                  data_oe,  // Asserted when CPU is reading from CRTC

    input  logic config_crt_i,              // Select VDU (0 = 12"/CRTC, 1 = 9"/non-CRTC)

    output logic h_sync_o,                  // Horizontal sync
    output logic v_sync_o,                  // Vertical sync
    output logic de_o,                      // Display enable

    output logic [13:0] ma_o,               // Refresh RAM address lines
    output logic [ 4:0] ra_o                // Raster address lines
);
    logic [ 7:0] h_total;
    logic [ 7:0] h_displayed;
    logic [ 7:0] h_sync_pos;
    logic [ 3:0] h_sync_width;
    logic [ 4:0] v_sync_width;
    logic [ 6:0] v_total;
    logic [ 4:0] v_adjust;
    logic [ 6:0] v_displayed;
    logic [ 6:0] v_sync_pos;
    logic [ 4:0] max_scan_line;
    logic [13:0] start_addr;

    video_crtc_reg video_crtc_reg (
        .wb_clock_i(wb_clock_i),
        .wbp_addr_i(wbp_addr_i),
        .wbp_data_i(wbp_data_i),
        .wbp_data_o(wbp_data_o),
        .wbp_we_i(wbp_we_i),
        .wbp_cycle_i(wbp_cycle_i),
        .wbp_strobe_i(wbp_strobe_i),
        .wbp_stall_o(wbp_stall_o),
        .wbp_ack_o(wbp_ack_o),

        .clk_en_i(clk_en_i),
        .data_i(data_i),
        .cs_i(cs_i),
        .we_i(we_i),
        .rs_i(rs_i),

        .config_crt_i(config_crt_i),
        
        .r0_h_total_o(h_total),
        .r1_h_displayed_o(h_displayed),
        .r2_h_sync_pos_o(h_sync_pos),
        .r3_h_sync_width_o(h_sync_width),
        .r3_v_sync_width_o(v_sync_width),
        .r4_v_total_o(v_total),
        .r5_v_adjust_o(v_adjust),
        .r6_v_displayed_o(v_displayed),
        .r7_v_sync_pos_o(v_sync_pos),
        .r9_max_scan_line_o(max_scan_line),
        .r1213_start_addr_o(start_addr)
    );

    // Horizontal

    logic [7:0] h_total_counter = '0;
    logic [3:0] h_sync_counter  = '0;
    logic       h_display       = 1'b1;
    logic       h_sync          = '0;

    wire last_column = h_total_counter == h_displayed;
    wire line_ending = h_total_counter == h_total;

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) h_total_counter <= '0;
        else if (clk_en_i) begin
            if (line_ending) h_total_counter <= '0;
            else h_total_counter <= h_total_counter + 1'b1;
        end
    end

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) h_display <= 1'b1;
        else if (last_column) h_display <= '0;
        else if (h_total_counter == '0) h_display <= 1'b1;
    end

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) h_sync <= '0;
        else if (h_sync_counter == h_sync_width) h_sync <= 1'b0;
        else if (h_total_counter == h_sync_pos) h_sync <= 1'b1;
    end

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) h_sync_counter <= '0;
        else if (clk_en_i) begin            
            if (h_sync) h_sync_counter <= h_sync_counter + 1'b1;
            else h_sync_counter <= '0;
        end
    end

    // Raster address generator

    logic [4:0] line_counter = '0;
    wire last_line  = line_counter == max_scan_line;
    wire row_ending = last_line && line_ending;

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) line_counter <= '0;
        else if (clk_en_i) begin
            if (frame_start) line_counter <= '0;
            else if (row_ending) line_counter <= '0;
            else if (line_ending) line_counter <= line_counter + 1'b1;
        end
    end

    // Vertical

    logic [6:0] v_total_counter = '0;
    logic [4:0] v_sync_counter  = '0;
    logic       v_display       = 1'b1;
    logic       v_sync          = '0;

    wire last_row = v_total_counter == v_total;
    wire [6:0] next_v_total_counter = frame_start ? '0 : row_ending ? v_total_counter + 1'b1 : v_total_counter;

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) v_total_counter <= '0;
        else if (clk_en_i) begin
            v_total_counter <= next_v_total_counter;
        end
    end

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) v_display <= 1'b1;
        else if (clk_en_i) begin
            if (next_v_total_counter == v_displayed) v_display <= '0;
            else if (frame_start) v_display <= 1'b1;
        end
    end

    // TODO: 'v_sync_latched' appears to prevent multiple VSYNC pulses if v_total_counter/v_sync_pos
    //       change during the current frame.  Why?
    logic v_sync_latched = '0;

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) begin
            v_sync <= '0;
            v_sync_latched <= '0;
        end else begin
            if (frame_start) v_sync_latched <= '0;
            if (v_sync_counter == v_sync_width) v_sync <= 1'b0;
            else if (v_total_counter == v_sync_pos && !v_sync_latched) begin
                v_sync_latched <= 1'b1;
                v_sync <= 1'b1;
            end
        end
    end

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) v_sync_counter <= '0;
        else if (clk_en_i) begin
            if (line_ending) begin
                if (v_sync) v_sync_counter <= v_sync_counter + 1'b1;
                else v_sync_counter <= '0;
            end
        end
    end

    // Frame

    logic [4:0] adjust_counter = '0;
    wire adjusting     = frame_state_d == ADJUSTING;
    wire adjust_ending = line_ending && adjust_counter == v_adjust;

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) adjust_counter <= '0;
        else if (clk_en_i) begin
            if (adjust_ending) adjust_counter <= '0;
            else if (adjusting && line_ending) adjust_counter <= adjust_counter + 1'b1;
        end
    end

    localparam
        NORMAL         = 3'b000,
        FRAME_ENDING   = 3'b001,
        ADJUST_PENDING = 3'b011,
        ADJUSTING      = 3'b101;
    
    logic [2:0] frame_state_d, frame_state_q = NORMAL;
    
    wire frame_ending = frame_state_q[0];
    wire frame_start = frame_ending && adjust_ending;

    always_comb begin
        // Do not move into 'default' case.
        frame_state_d = frame_state_q;
        
        unique case (frame_state_q)
            NORMAL: if (last_row && last_line) frame_state_d = FRAME_ENDING;

            FRAME_ENDING: begin
                if (v_adjust != '0) frame_state_d = ADJUST_PENDING;
                else if (line_ending) frame_state_d = NORMAL;
            end

            ADJUST_PENDING: if (line_ending) frame_state_d = ADJUSTING;
            ADJUSTING: if (adjust_ending) frame_state_d = NORMAL;
            default: begin
                // synthesis off
                $fatal(1, "Illegal 'frame_state_q' value: %0d", frame_state_q);
                // synthesis on
            end
        endcase
    end

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) frame_state_q <= NORMAL;
        else if (clk_en_i)frame_state_q <= frame_state_d;
    end

    // Linear address generator

    logic [13:0] row_addr = '0;

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) row_addr <= '0;
        else if (clk_en_i) begin
            if (frame_start) row_addr <= start_addr;
            else if (last_line && last_column) row_addr <= ma_o;
        end
    end

    always_ff @(posedge wb_clock_i) begin
        if (reset_i) ma_o <= '0;
        else if (clk_en_i) begin
            if (frame_start) ma_o <= start_addr;
            else if (line_ending) ma_o <= row_addr;
            else ma_o <= ma_o + 1'b1;
        end
    end

    assign h_sync_o    = h_sync;
    assign v_sync_o    = v_sync; 
    assign de_o        = h_display && v_display;
    assign ra_o        = line_counter;
endmodule
