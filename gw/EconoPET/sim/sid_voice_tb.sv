// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

module sid_voice_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic       write_en = 1'b0;
    logic [4:0] addr = '0;
    logic [7:0] data = '0;
    logic [11:0] voice_out;

    // clkEn is held low so the phase accumulator stays at its $555555 reset
    // value, making the pulse comparator's output depend only on the writes.
    sid_voice sid_voice (
        .clk(clock),
        .clkEn(1'b0),
        .iRst(1'b0),
        .iWE(write_en),
        .iAddr(addr),
        .iData(data),
        .iExtMSB(1'b0),
        .oMSB(),
        .oOut(voice_out)
    );

    // Drive writes before the rising edge that samples them.
    task write_register (
        input logic [4:0] reg_addr,
        input logic [7:0] reg_data
    );
        @(negedge clock);
        addr = reg_addr;
        data = reg_data;
        write_en = 1'b1;
        @(posedge clock);
        #1;
        write_en = 1'b0;
    endtask

    // Advance through the pulse comparator and output mixer registers.
    task advance_pulse_output;
        repeat (2) begin
            @(posedge clock);
            #1;
        end
    endtask

    // Exercise pulse widths below, above, and equal to the fixed phase.
    task run;
        // Enable the pulse waveform. The phase stays at $555 because clkEn is low.
        write_register(5'h04, 8'h40);

        // At phase $555, PW=$600 selects low.
        write_register(5'h02, 8'h00);
        write_register(5'h03, 8'h06);
        advance_pulse_output();
        `assert_equal(voice_out, 12'h000)

        // At phase $555, PW=$400 selects high.
        write_register(5'h03, 8'h04);
        advance_pulse_output();
        `assert_equal(voice_out, 12'hfff)

        // Equality selects low because the pulse comparator uses <=.
        write_register(5'h02, 8'h55);
        write_register(5'h03, 8'h05);
        advance_pulse_output();
        `assert_equal(voice_out, 12'h000)
    endtask

    `TB_INIT
endmodule
