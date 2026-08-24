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

    task write_register (
        input logic [4:0] reg_addr,
        input logic [7:0] reg_data
    );
        @(posedge clock);
        addr = reg_addr;
        data = reg_data;
        write_en = 1'b1;
        @(negedge clock);
        #1;
        write_en = 1'b0;
    endtask

    task run;
        // phase[23:12] starts at $555. Set PW=$600, which selects low.
        write_register(5'h02, 8'h00);
        write_register(5'h03, 8'h06);

        @(posedge clock);
        #1;
        `assert_equal(sid_voice.regPWPos, 12'h600)
        `assert_equal(sid_voice.wavPulse, 12'hfff)

        @(posedge clock);
        #1;
        `assert_equal(sid_voice.wavPulse, 12'h000)

        // Set PW=$400, which selects high. The comparator result changes one
        // full clock after the shadow register captures the write.
        write_register(5'h03, 8'h04);

        @(posedge clock);
        #1;
        `assert_equal(sid_voice.regPWPos, 12'h400)
        `assert_equal(sid_voice.wavPulse, 12'h000)

        @(posedge clock);
        #1;
        `assert_equal(sid_voice.wavPulse, 12'hfff)

        // Equality selects low because the pulse comparator uses <=.
        write_register(5'h02, 8'h55);
        write_register(5'h03, 8'h05);

        @(posedge clock);
        #1;
        `assert_equal(sid_voice.regPWPos, 12'h555)
        `assert_equal(sid_voice.wavPulse, 12'hfff)

        @(posedge clock);
        #1;
        `assert_equal(sid_voice.wavPulse, 12'h000)
    endtask

    `TB_INIT
endmodule
