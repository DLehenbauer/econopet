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

module video_crtc_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic cpu_grant;
    logic spi_grant;
    logic video_grant;
    logic grant_strobe;

    timing timing (
        .clock_i(clock),
        .cpu_grant_o(cpu_grant),
        .video_grant_o(video_grant),
        .spi_grant_o(spi_grant),
        .strobe_o(grant_strobe)
    );
    
    wire cclk_en = cpu_grant & grant_strobe;

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
        .reset_i(res),
        .sys_clock_i(clock),
        .wr_strobe_i(cclk_en),
        .cclk_en_i(cclk_en),
        .cs_i(cs),                      // CRTC selected for data transfer (driven by address decoding)
        .rw_ni(!we),                    // Direction of date transfers (0 = writing to CRTC, 1 = reading from CRTC)
        .rs_i(rs),                      // Register select (0 = write address/read status, 1 = read addressed register)
        .data_i(crtc_data_i),           // Transfer data written from CPU to CRTC when CS asserted and /RW is low
        .data_o(crtc_data_o),           // Transfer data read by CPU from CRTC when CS asserted and /RW is high
        .data_oe(crtc_data_oe),         // Asserted when CPU is reading from CRTC
        .h_sync_o(h_sync),              // Horizontal sync
        .v_sync_o(v_sync),              // Vertical sync
        .de_o(de),                      // Display enable
        .ma_o(ma),                      // Refresh RAM address lines
        .ra_o(ra)                       // Raster address lines
    );

    task crtc_begin(
        input logic rs_i,
        input logic we_i,
        input logic [7:0] data_i = 8'hxx
    );
        @(posedge cclk_en);
        cs = 1'b1;
        rs = rs_i;
        we = we_i;
        crtc_data_i = data_i;

        @(posedge cclk_en);
    endtask

    task crtc_end;
        if (cclk_en) @(negedge cclk_en);

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
        @(negedge cclk_en);
        res = 1'b1;
        @(posedge cclk_en);
        @(negedge cclk_en);
        res = '0;
    endtask

    function real to_hz(input real elapsed);
        real s, hz;

        s = elapsed * (1.0e3 / 1.0e12);
        hz = 1.0 / s;
        return hz;
    endfunction

    task run;
        bit [63:0] start_time, elapsed_time;

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
        start_time = $time;

        @(posedge h_sync);
        $display("[%t] HSYNC at %0.2f kHz", $time, to_hz($time - start_time)/1000.0);

        // Measure Vertical Sync Frequency
        @(posedge v_sync);
        start_time = $time;

        @(posedge v_sync);
        $display("[%t] VSYNC at %0.2f Hz", $time, to_hz($time - start_time));

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
