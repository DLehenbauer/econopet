// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

module audio_tb;
    logic clock = 1'b0;
    always #5 clock = ~clock;
    logic reset = 1'b1;
    logic enable = 1'b1;
    logic cb2 = 1'b0;
    logic wr = 1'b0;
    wire left, right;

    audio dut (
        .reset_i(reset), .sys_clock_i(clock), .clk1_en_i(1'b0),
        .sid_en_i(1'b1), .cpu_wr_strobe_i(wr),
        .addr_i(5'h18), .data_i(8'h0f), .data_o(),
        .diag_i(1'b1), .via_cb2_i(cb2), .audio_enable_i(enable),
        .audio_l_o(left), .audio_r_o(right)
    );

    // Exercise mute with both polarities while checking that DAC and SID stay live.
    task run;
        repeat (16) @(negedge clock);
        reset = 1'b0;
        wr = 1'b1;
        @(negedge clock);
        wr = 1'b0;
        for (int polarity = 0; polarity < 2; polarity++) begin
            cb2 = 1'(polarity);
            enable = 1'b1;
            #1;
            `assert_equal(dut.dac_sample, dut.mixed)
            `assert_compare(dut.mixed, !=, 16'sd0)
            enable = 1'b0;
            #1;
            `assert_equal(dut.dac_sample, 16'sd0)
            repeat (4) begin
                logic previous_left;
                @(negedge clock);
                previous_left = left;
                @(negedge clock);
                `assert_equal(left, !previous_left)
                `assert_equal(right, left)
                `assert_equal(dut.sid.regVolume, 4'hf)
            end
            enable = 1'b1;
            #1;
            `assert_equal(dut.dac_sample, dut.mixed)
        end
    endtask

    `TB_INIT
endmodule
