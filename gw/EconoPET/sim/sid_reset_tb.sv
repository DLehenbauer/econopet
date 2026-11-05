// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

module sid_reset_tb;
    logic clock = 1'b0;
    always #5 clock = ~clock;

    logic reset = 1'b0;
    logic clk_en = 1'b0;
    logic write_en = 1'b0;
    logic [4:0] addr = '0;
    logic [7:0] data = '0;
    wire [7:0] data_out;
    wire signed [15:0] sid_out;
    tri pot_x;
    tri pot_y;
    assign pot_x = 1'bz;
    assign pot_y = 1'bz;

    sid #(
        .POT_SUPPORT(0),
        .CURVE_6581_FILE("external/icesid/icesid/curve_6581.hex")
    ) dut (
        .clk(clock),
        .clkEn(clk_en),
        .iRst(reset),
        .iWE(write_en),
        .iAddr(addr),
        .iDataW(data),
        .oDataR(data_out),
        .oOut(sid_out)
`ifndef VERILATOR
        ,
        .ioPotX(pot_x),
        .ioPotY(pot_y)
`endif
    );

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

    task run;
        write_register(5'h00, 8'h34);
        write_register(5'h01, 8'h01);
        write_register(5'h02, 8'h56);
        write_register(5'h03, 8'h03);
        write_register(5'h04, 8'h41);
        write_register(5'h05, 8'h12);
        write_register(5'h06, 8'h34);
        write_register(5'h15, 8'h07);
        write_register(5'h16, 8'h56);
        write_register(5'h17, 8'hf7);
        write_register(5'h18, 8'hff);

        `assert_equal(dut.voices.voice0.regFreq, 16'h0134)
        `assert_equal(dut.voices.voice0.regPW, 12'h356)
        `assert_equal(dut.voices.voice0.regPulse, 1'b1)
        `assert_equal(dut.env0.regGate, 1'b1)
        `assert_equal(dut.env0.regAtt, 4'h1)
        `assert_equal(dut.env0.regDec, 4'h2)
        `assert_equal(dut.sid_filter.regFreq, 11'h2b7)
        `assert_equal(dut.sid_filter.regRes, 4'hf)
        `assert_equal(dut.regVolume, 4'hf)

        // Confirm the programmed SID produces a nonzero sample before reset.
        dut.env0.impl.env = 8'hff;
        clk_en = 1'b1;
        repeat (8) @(posedge clock);
        #1;
        `assert_compare(sid_out, !=, 16'sd0)
        clk_en = 1'b0;

        // reSID preserves oscillator phase and envelope/rate counters on reset.
        dut.voices.voice0.phase = 24'h1a3456;
        dut.env0.impl.env = 8'h5a;
        dut.env0.impl.cnt = 15'd123;
        dut.env0.impl.divMax = 5'd29;
        dut.env0.impl.div = 5'd17;
        dut.sid_filter.low = 17'sd1234;
        dut.sid_filter.high = -17'sd2345;
        dut.sid_filter.band = 17'sd3456;

        // Reset must win over a simultaneous register write.
        reset = 1'b1;
        write_en = 1'b1;
        addr = 5'h18;
        data = 8'hff;
        // Hold reset long enough for the reset volume to flush through the audio pipeline.
        repeat (12) @(posedge clock);
        #1;

        `assert_equal(dut.voices.voice0.regFreq, 16'h0000)
        `assert_equal(dut.voices.voice0.regPW, 12'h000)
        `assert_equal(dut.voices.voice0.regPulse, 1'b0)
        `assert_equal(dut.env0.regGate, 1'b0)
        `assert_equal(dut.env0.regAtt, 4'h0)
        `assert_equal(dut.env0.regDec, 4'h0)
        `assert_equal(dut.sid_filter.regFreq, 11'h000)
        `assert_equal(dut.sid_filter.regRes, 4'h0)
        `assert_equal(dut.regFilt, 3'h0)
        `assert_equal(dut.regMode, 3'h0)
        `assert_equal(dut.regVolume, 4'h0)
        `assert_equal(dut.regLastWrite, 8'h00)
        `assert_equal(dut.voices.voice0.phase, 24'h1a3456)
        `assert_equal(dut.voices.voice0.lfsr, 23'h7ffffe)
        `assert_equal(dut.voices.voice0.noiseClkLag, 1'b1)
        `assert_equal(dut.env0.impl.env, 8'h5a)
        `assert_equal(dut.env0.impl.cnt, 15'd123)
        `assert_equal(dut.env0.impl.divMax, 5'd29)
        `assert_equal(dut.env0.impl.div, 5'd0)
        `assert_equal(sid_out, 16'sd0)

        write_en = 1'b0;
        reset = 1'b0;
        repeat (4) begin
            @(posedge clock);
            #1;
            `assert_equal(sid_out, 16'sd0)
        end

        // A preserved high phase bit must not create a spurious LFSR clock after reset.
        clk_en = 1'b1;
        repeat (4) @(posedge clock);
        #1;
        `assert_equal(dut.voices.voice0.lfsr, 23'h7ffffe)
    endtask

    `TB_INIT
endmodule
