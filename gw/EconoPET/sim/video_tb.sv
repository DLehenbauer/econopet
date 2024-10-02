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
    logic [    DATA_WIDTH-1:0] wbc_din      = 8'haa;    // Test pattern
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

    logic clk1n_en;
    logic clk8_en;
    logic clk16_en;

    timing timing (
        .sys_clock_i(clock),
        .clk1n_en_o(clk1n_en),
        .clk8_en_o(clk8_en),
        .clk16_en_o(clk16_en)
    );

    logic        crtc_res;
    logic        crtc_cs;
    logic        crtc_we;
    logic        crtc_rs;
    logic [7:0]  crtc_data_i;
    logic [7:0]  crtc_data_o;
    logic        crtc_data_oe;
    logic        h_sync;
    logic        v_sync;
    logic        dotgen_video;

    video video (
        .clk8_en_i(clk8_en),
        .clk16_en_i(clk16_en),
        .clk1_en_i(clk1n_en),

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

        // CRTC
        .cpu_reset_i(crtc_res),
        .crtc_clk_en_i(clk1n_en),
        .crtc_cs_i(crtc_cs),
        .crtc_we_i(crtc_we),
        .crtc_rs_i(crtc_rs),
        .crtc_data_i(crtc_data_i),
        .crtc_data_o(crtc_data_o),
        .crtc_data_oe(crtc_data_oe),

        .col_80_mode_i(1'b1),
        .graphic_i(1'b0),

        .v_sync_o(v_sync),
        .h_sync_o(h_sync),
        .video_o(dotgen_video)
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

    task crtc_begin(
        input logic rs_i,
        input logic we_i,
        input logic [7:0] data_i = 8'hxx
    );
        @(posedge clk1n_en);
        crtc_cs = 1'b1;
        crtc_rs = rs_i;
        crtc_we = we_i;
        crtc_data_i = data_i;

        @(posedge clk1n_en);
    endtask

    task crtc_end;
        if (clk1n_en) @(negedge clk1n_en);

        #1;

        crtc_cs = '0;
        crtc_we = '0;
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
        logic [DATA_WIDTH-1:0] value;

        $display("[%t] BEGIN %m", $time);

        wb.reset;

        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            wb.read(common_pkg::wb_crtc_addr(r), value);
            $display("[%t]   R%0d = %d", $time, r, value);
        end

        setup('{
            8'd5,       // H Total:      Width of scanline in characters (-1)
            8'd3,       // H Displayed:  Number of characters displayed per scanline
            8'd4,       // H Sync Pos:   Start of horizontal sync pulse in characters
            8'h11,      // Sync Width:   H. Sync = 1 char, V. Sync = 1 scanline
            8'd4,       // V Total:      Height of frame in characters (-1)
            8'd2,       // V Adjust:     Adjustment of frame height in scanlines
            8'd2,       // V Displayed:  Number of characters displayed per frame
            8'd3,       // V Sync Pos:   Position of vertical sync pulse in characters
            8'h00,      // Mode Control: (Unused)
            8'h02,      // Char Height:  Height of one character in scanlines (-1)
            8'h00,      // Cursor Start: (Unused)
            8'h00,      // Cursor End:   (Unused)
            8'h00,      // Display H:    Display start address ([3:0] high bits)
            8'h00       // Display L:    Display start address (low bits)
        });

        @(posedge v_sync);
        @(posedge v_sync);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
