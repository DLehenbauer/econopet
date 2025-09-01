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

module video_crtc_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    stopwatch stopwatch();

    logic clk1n_en;
    logic clk8_en;
    logic clk16_en;

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
    
    logic        res;
    logic        cs;
    logic        we;
    logic        rs;
    logic [7:0]  crtc_data_i;
    logic [7:0]  crtc_data_o;
    logic        crtc_data_oe;
    logic        de;
    logic [13:0] ma;
    logic [4:0]  ra;
    logic        h_sync;
    logic        v_sync;

    video_crtc video_crtc (
        .wb_clock_i(clock),
        .wbp_addr_i(),           // Wishbone unused for this testbench
        .wbp_data_o(),
        .wbp_we_i(),
        .wbp_cycle_i(),
        .wbp_strobe_i(),
        .wbp_stall_o(),
        .wbp_ack_o(),

        .reset_i(res),
        .clk_en_i(clk1n_en),
        .cs_i(cs),
        .we_i(we),
        .rs_i(rs),
        .data_i(crtc_data_i),
        .data_o(crtc_data_o),
        .data_oe(crtc_data_oe),
        .h_sync_o(h_sync),
        .v_sync_o(v_sync),
        .de_o(de),
        .ma_o(ma),
        .ra_o(ra)
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

    // task assert(input logic [7:0] expected);
    //     crtc_begin(/* rs: */ '1, /* we: */ '0);
        
    //     assert(crtc_data_o == expected) else begin
    //         $error("Selected CRTC register must be %d, but got %d.", expected, crtc_data);
    //         $finish;
    //     end

    //     crtc_end();
    // endtask

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

    task reset;
        @(negedge clk1n_en);
        res = 1'b1;
        @(posedge clk1n_en);
        @(negedge clk1n_en);
        res = '0;
    endtask

    task run;
        $display("[%t] BEGIN %m", $time);

        reset();

        if (1) begin
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
        end else begin
            setup('{
                8'd49,      // H Total:      Width of scanline in characters (-1)
                8'd40,      // H Displayed:  Number of characters displayed per scanline
                8'd41,      // H Sync Pos:   Start of horizontal sync pulse in characters
                8'h0f,      // Sync Width:   H. Sync = 15 char, V. Sync = 16 scanline
                8'd40,      // V Total:      Height of frame in characters (-1)
                8'd05,      // V Adjust:     Adjustment of frame height in scanlines
                8'd25,      // V Displayed:  Number of characters displayed per frame
                8'd33,      // V Sync Pos:   Position of vertical sync pulse in characters
                8'd00,      // Mode Control: (Unused)
                8'd07,      // Char Height:  Height of one character in scanlines (-1)
                8'h00,      // Cursor Start: (Unused)
                8'h00,      // Cursor End:   (Unused)
                8'h00,      // Display H:    Display start address ([3:0] high bits)
                8'h00       // Display L:    Display start address (low bits)
            });
        end

        reset();

        // Measure Horizontal Sync Frequency
        @(posedge h_sync);
        stopwatch.start();

        @(posedge h_sync);
        $display("[%t] HSYNC at %0.2f kHz", $time, stopwatch.freq_khz());

        // Measure Vertical Sync Frequency
        @(posedge v_sync);
        stopwatch.start();

        @(posedge v_sync);
        $display("[%t] VSYNC at %0.2f Hz", $time, stopwatch.freq_hz());

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
