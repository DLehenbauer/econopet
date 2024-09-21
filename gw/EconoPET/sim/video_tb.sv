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

    logic [ WB_ADDR_WIDTH-1:0] addr;
    logic [    DATA_WIDTH-1:0] dout;
    logic [    DATA_WIDTH-1:0] din;
    logic                      we;
    logic                      cycle;
    logic                      strobe;
    logic                      stall = 1'b0;
    logic                      ack   = 1'b1;

    logic video_grant;
    logic clk1_en;
    logic clk8_en;
    logic clk16_en;

    logic v_sync;
    logic h_sync;
    logic video_o;

    timing timing (
        .clock_i(clock),
        .cpu_grant_o(),
        .video_grant_o(video_grant),
        .spi_grant_o(),
        .clk1_en_o(clk1_en),
        .clk8_en_o(clk8_en),
        .clk16_en_o(clk16_en),
        .strobe_o()
    );

    video video (
        .clk1_en_i(clk1_en),
        .clk8_en_i(clk8_en),
        .clk16_en_i(clk16_en),

        .wb_clock_i(clock),
        .wb_addr_o(addr),
        .wb_data_i(8'h55),  // Test pattern
        .wb_data_o(din),
        .wb_we_o(we),
        .wb_cycle_o(cycle),
        .wb_strobe_o(strobe),
        .wb_stall_i(stall),
        .wb_ack_i(ack),

        // We leave CRTC at it's default settings for this testbench.
        .cpu_reset_i(1'b0),
        .crtc_cs_i(1'b0),
        .crtc_we_i(1'b0),
        .crtc_rs_i(1'b0),
        .crtc_data_i(8'hxx),
        .crtc_data_o(),
        .crtc_data_oe(),
        .wr_strobe_i(1'b0),

        .col_80_mode_i(1'b1),
        .graphic_i(1'b0),

        .v_sync_o(v_sync),
        .h_sync_o(h_sync),
        .video_o(video_o)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        @(posedge v_sync);
        @(posedge v_sync);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
