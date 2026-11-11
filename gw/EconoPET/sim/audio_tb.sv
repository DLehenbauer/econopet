// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

module audio_tb;
    localparam logic [15:0] VOICE_FREQUENCY = 16'h0134;
    localparam logic [7:0] MUTED_VOLUME = 8'h07;

    logic clock = 1'b0;
    always #5 clock = ~clock;
    logic reset = 1'b1;
    logic clk_en = 1'b0;
    logic enable = 1'b1;
    logic cb2 = 1'b0;
    logic wr = 1'b0;
    logic [SID_ADDR_REG_WIDTH-1:0] addr = '0;
    logic [7:0] data = '0;
    wire left, right;

    audio dut (
        .reset_i(reset), .sys_clock_i(clock), .clk1_en_i(clk_en),
        .sid_en_i(1'b1), .cpu_wr_strobe_i(wr),
        .addr_i(addr), .data_i(data), .data_o(),
        .diag_i(1'b1), .via_cb2_i(cb2), .audio_enable_i(enable),
        .audio_l_o(left), .audio_r_o(right)
    );

    // Write through the audio module's normal CPU-to-SID path.
    task write_register(
        input logic [SID_ADDR_REG_WIDTH-1:0] reg_addr,
        input logic [7:0] reg_data
    );
        @(negedge clock);
        addr = reg_addr;
        data = reg_data;
        wr = 1'b1;
        @(negedge clock);
        wr = 1'b0;
    endtask

    // Advance one 1 MHz SID tick and verify exact oscillator continuity.
    task step_voice;
        logic [23:0] previous_phase;
        @(negedge clock);
        previous_phase = dut.sid.voices.voice0.phase;
        clk_en = 1'b1;
        @(negedge clock);
        clk_en = 1'b0;
        `assert_equal(dut.sid.voices.voice0.phase, previous_phase + {8'd0, VOICE_FREQUENCY})
        repeat (63) @(negedge clock);
        `assert_equal(dut.sid.voices.voice0.phase, previous_phase + {8'd0, VOICE_FREQUENCY})
    endtask

    // Exercise mute with both polarities while checking that DAC and SID stay live.
    task run;
        logic [23:0] muted_phase;

        // Establish a progressing voice before changing the output-only control.
        repeat (16) @(negedge clock);
        reset = 1'b0;
        write_register(SID_R0_VOICE1_FREQ_LO, VOICE_FREQUENCY[7:0]);
        write_register(SID_R1_VOICE1_FREQ_HI, VOICE_FREQUENCY[15:8]);
        write_register(SID_R24_MODE_VOL, 8'h0f);
        step_voice;

        for (int polarity = 0; polarity < 2; polarity++) begin
            // Mute the mixed signal, not either input's state or clock.
            cb2 = 1'(polarity);
            enable = 1'b1;
            #1;
            `assert_equal(dut.dac_sample, dut.mixed)
            `assert_compare(dut.mixed, !=, 16'sd0)
            enable = 1'b0;
            #1;
            `assert_equal(dut.dac_sample, 16'sd0)
            write_register(SID_R24_MODE_VOL, MUTED_VOLUME);
            `assert_equal(dut.sid.regVolume, MUTED_VOLUME[3:0])
            repeat (2) step_voice;
            `assert_equal(dut.dac_sample, 16'sd0)

            // A zero signed sample must still produce a balanced DAC waveform.
            repeat (4) begin
                logic previous_left;
                @(negedge clock);
                previous_left = left;
                @(negedge clock);
                `assert_equal(left, !previous_left)
                `assert_equal(right, left)
                `assert_equal(dut.sid.regVolume, MUTED_VOLUME[3:0])
            end

            // Unmuting must preserve the phase reached while inaudible.
            muted_phase = dut.sid.voices.voice0.phase;
            enable = 1'b1;
            #1;
            `assert_equal(dut.sid.voices.voice0.phase, muted_phase)
            `assert_equal(dut.dac_sample, dut.mixed)
            step_voice;
        end
    endtask

    `TB_INIT
endmodule
