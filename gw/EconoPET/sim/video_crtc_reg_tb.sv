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

`include "./sim/assert.svh"

import common_pkg::*;

module video_crtc_reg_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    stopwatch stopwatch();

    logic [ WB_ADDR_WIDTH-1:0] wb_addr;
    logic [    DATA_WIDTH-1:0] wb_crtc_dout;
    logic wb_we;
    logic wb_cycle;
    logic wb_strobe;
    logic wb_stall;
    logic wb_ack;

    logic [DATA_WIDTH-1:0] wb_data_out;

    wb_driver wb (
        .wb_clock_i(clock),
        .wb_addr_o(wb_addr),
        .wb_data_i(wb_crtc_dout),
        .wb_data_o(wb_data_out),
        .wb_we_o(wb_we),
        .wb_cycle_o(wb_cycle),
        .wb_strobe_o(wb_strobe),
        .wb_ack_i(wb_ack),
        .wb_stall_i(wb_stall)
    );

    logic clk16_en;
    logic clk8_en;
    logic clk1n_en;

    timing timing (
        .sys_clock_i(clock),
        .clk16_en_o(clk16_en),
        .clk8_en_o(clk8_en),
        .cpu_be_o(),
        .cpu_clock_o(),
        .cpu_data_strobe_o(clk1n_en),
        .load_sr1_o(),
        .load_sr2_o(),
        .grant_o(),
        .grant_valid_o()
    );

    logic        cs;
    logic        we;
    logic        rs;
    logic [7:0]  crtc_data_i;

    logic [ 7:0] r0_h_total;
    logic [ 7:0] r1_h_displayed;
    logic [ 7:0] r2_h_sync_pos;
    logic [ 3:0] r3_h_sync_width;
    logic [ 4:0] r3_v_sync_width;
    logic [ 6:0] r4_v_total;
    logic [ 4:0] r5_v_adjust;
    logic [ 6:0] r6_v_displayed;
    logic [ 6:0] r7_v_sync_pos;
    logic [ 4:0] r9_max_scan_line;
    logic [13:0] r1213_start_addr;

    video_crtc_reg video_crtc_reg (
        .wb_clock_i(clock),
        .wbp_addr_i(wb_addr),
        .wbp_data_i(wb_data_out),
        .wbp_data_o(wb_crtc_dout),
        .wbp_we_i(wb_we),
        .wbp_cycle_i(wb_cycle),
        .wbp_strobe_i(wb_strobe),
        .wbp_stall_o(wb_stall),
        .wbp_ack_o(wb_ack),
        .wbp_sel_i(1'b1),

        .clk_en_i(clk1n_en),
        .cs_i(cs),
        .we_i(we),
        .rs_i(rs),
        .data_i(crtc_data_i),
        .config_crt_i(1'b0),

        .r0_h_total_o(r0_h_total),
        .r1_h_displayed_o(r1_h_displayed),
        .r2_h_sync_pos_o(r2_h_sync_pos),
        .r3_h_sync_width_o(r3_h_sync_width),
        .r3_v_sync_width_o(r3_v_sync_width),
        .r4_v_total_o(r4_v_total),
        .r5_v_adjust_o(r5_v_adjust),
        .r6_v_displayed_o(r6_v_displayed),
        .r7_v_sync_pos_o(r7_v_sync_pos),
        .r9_max_scan_line_o(r9_max_scan_line),
        .r1213_start_addr_o(r1213_start_addr)
    );

    task crtc_begin(
        input logic rs_i,
        input logic we_i,
        input logic [7:0] data_i = 8'hxx
    );
        @(posedge clk1n_en);
        cs = 1'b1;
        rs = rs_i;
        we = we_i;
        crtc_data_i = data_i;

        @(posedge clk1n_en);
    endtask

    task crtc_end;
        if (clk1n_en) @(negedge clk1n_en);

        #1;

        cs = '0;
        we = '0;
        crtc_data_i = 8'hxx;
    endtask

    task select(input logic [7:0] register);
        crtc_begin(/* rs: */ '0, /* we: */ '1, /* data: */ register);
        crtc_end();
    endtask

    task write(input logic [7:0] data);
        crtc_begin(/* rs: */ '1, /* we: */ '1, /* data: */ data);
        crtc_end();
    endtask

    task cpu_write(input logic [7:0] register, input logic [7:0] data);
        select(register);
        write(data);
    endtask

    task setup(
        input logic [7:0] values[]
    );
        integer i;

        foreach(values[i]) begin
            select(/* register: */ i);
            write(/* data: */ values[i]);
            //crtc_assert(/* expected: */ values[i]);
        end
    endtask

    task run;
        integer r;
        logic [7:0] data;

        $display("[%t] BEGIN %m", $time);

        wb.reset;
        cs = '0;
        we = '0;

        // Test 1: Write and read back all CRTC registers via Wishbone
        $display("[%t]   Test 1: Wishbone write, Wishbone read", $time);
        // Use value (r + 0xA0) for each register - distinct from 9" and 12" defaults
        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            wb.write(common_pkg::wb_crtc_addr(r), 8'hA0 + r);
        end

        @(posedge clock);

        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            wb.read(common_pkg::wb_crtc_addr(r), data);
            `assert_equal(data, 8'hA0 + r);
        end

        // Verify register outputs are correctly updated, accounting for truncation        
        `assert_equal(r0_h_total, 8'hA0);                   // R0[7:0]
        `assert_equal(r1_h_displayed, 8'hA1);               // R1[7:0]
        `assert_equal(r2_h_sync_pos, 8'hA2);                // R2[7:0]
        `assert_equal(r3_h_sync_width, 4'h3);               // R3[3:0]
        `assert_equal(r3_v_sync_width, 5'hA);               // R3[7:4]
        `assert_equal(r4_v_total, 7'h24);                   // R4[6:0]
        `assert_equal(r5_v_adjust, 5'h05);                  // R5[4:0]
        `assert_equal(r6_v_displayed, 7'h26);               // R6[6:0]
        `assert_equal(r7_v_sync_pos, 7'h27);                // R7[6:0]
        `assert_equal(r9_max_scan_line, 5'h09);             // R9[4:0]
        `assert_equal(r1213_start_addr, {6'h2C, 8'hAD});    // { R12[5:0], R13[7:0] }

        // Test 2: CPU write, Wishbone read
        $display("[%t]   Test 2: CPU write, Wishbone read", $time);
        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            cpu_write(r, 8'hB0 + r);
        end

        @(posedge clock);

        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            wb.read(common_pkg::wb_crtc_addr(r), data);
            `assert_equal(data, 8'hB0 + r);
        end

        // Verify register outputs are correctly updated with new values
        `assert_equal(r0_h_total, 8'hB0);                   // R0[7:0]
        `assert_equal(r1_h_displayed, 8'hB1);               // R1[7:0]
        `assert_equal(r2_h_sync_pos, 8'hB2);                // R2[7:0]
        `assert_equal(r3_h_sync_width, 4'h3);               // R3[3:0]
        `assert_equal(r3_v_sync_width, 5'h0B);              // R3[7:4]
        `assert_equal(r4_v_total, 7'h34);                   // R4[6:0]
        `assert_equal(r5_v_adjust, 5'h15);                  // R5[4:0]
        `assert_equal(r6_v_displayed, 7'h36);               // R6[6:0]
        `assert_equal(r7_v_sync_pos, 7'h37);                // R7[6:0]
        `assert_equal(r9_max_scan_line, 5'h19);             // R9[4:0]
        `assert_equal(r1213_start_addr, {6'h3C, 8'hBD});    // { R12[5:0], R13[7:0] }

        #1 $display("[%t] END %m", $time);
    endtask
endmodule