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
`include "./src/common_pkg.svh"

import common_pkg::*;

module video_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic [ WB_ADDR_WIDTH-1:0] wbc_addr;
    logic [    DATA_WIDTH-1:0] wbc_din      = 8'h55;    // Test pattern
    logic                      wbc_we;
    logic                      wbc_cycle;
    logic                      wbc_strobe;
    logic                      wbc_stall    = 1'b0;
    logic                      wbc_ack      = 1'b1;

    logic [ WB_ADDR_WIDTH-1:0] wbp_addr;
    logic [    DATA_WIDTH-1:0] wbp_dout;
    logic                      wbp_we;
    logic                      wbp_cycle;
    logic                      wbp_strobe;
    logic                      wbp_stall;
    logic                      wbp_ack;

    logic clk1_en;
    logic clk8_en;
    logic clk16_en;

    logic v_sync;
    logic h_sync;
    logic video_o;

    timing timing (
        .clock_i(clock),
        .clk1_en_o(clk1_en),
        .clk8_en_o(clk8_en),
        .clk16_en_o(clk16_en)
    );

    video video (
        .clk1_en_i(clk1_en),
        .clk8_en_i(clk8_en),
        .clk16_en_i(clk16_en),

        .config_crt_i(1'b0),    // 0 = 12"/CRTC, 1 = 9"/non-CRTC

        // Wishbone controller
        .wb_clock_i(clock),
        .wb_addr_o(wbc_addr),
        .wb_data_i(wbc_din),
        .wb_we_o(wbc_we),
        .wb_cycle_o(wbc_cycle),
        .wb_strobe_o(wbc_strobe),
        .wb_stall_i(wbc_stall),
        .wb_ack_i(wbc_ack),

        // Wishbone peripheral
        .wb_addr_i(wbp_addr),
        .wb_data_o(wbp_dout),
        .wb_we_i(wbp_we),
        .wb_cycle_i(wbp_cycle),
        .wb_strobe_i(wbp_strobe),
        .wb_stall_o(wbp_stall),
        .wb_ack_o(wbp_ack),

        // We leave CRTC at it's default settings for this testbench.
        .cpu_reset_i(1'b0),
        .crtc_clk_en_i(clk1_en),
        .crtc_cs_i(1'b0),
        .crtc_we_i(1'b0),
        .crtc_rs_i(1'b0),
        .crtc_data_i(8'hxx),
        .crtc_data_o(),
        .crtc_data_oe(),

        .col_80_mode_i(1'b1),
        .graphic_i(1'b0),

        .v_sync_o(v_sync),
        .h_sync_o(h_sync),
        .video_o(video_o)
    );

    wb_driver wb (
        .wb_clock_i(clock),
        .wb_addr_o(wbp_addr),
        .wb_data_i(wbp_dout),
        .wb_data_o(),
        .wb_we_o(wbp_we),
        .wb_cycle_o(wbp_cycle),
        .wb_strobe_o(wbp_strobe),
        .wb_ack_i(wbp_ack),
        .wb_stall_i(wbp_stall)
    );

    task run;
        integer r;
        bit [DATA_WIDTH-1:0] value;

        $display("[%t] BEGIN %m", $time);

        wb.reset;

        for (r = 0; r <= 13; r = r + 1) begin
            $display("reading %d", r);
            wb.read(common_pkg::wb_crtc_addr(r), value);
            $display("[%t]   R%d = %d", $time, r, value);
        end

        $finish;

        @(posedge v_sync);
        @(posedge v_sync);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
