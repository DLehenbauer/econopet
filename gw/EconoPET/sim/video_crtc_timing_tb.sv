// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

module video_crtc_timing_tb;
    localparam time US = 1us;

    typedef struct packed {
        logic [15:0] h_sync_width_us;
        logic [15:0] h_sync_period_us;
        logic [15:0] h_sync_to_de_us;
        logic [15:0] de_width_us;
        logic [15:0] v_sync_width_us;
        logic [15:0] v_sync_period_us;
        logic [15:0] v_sync_to_h_sync_us;
        logic [6:0] v_sync_rise_row;
        logic [4:0] v_sync_rise_line;
        logic [7:0] v_sync_rise_column;
        logic [6:0] v_sync_fall_row;
        logic [4:0] v_sync_fall_line;
        logic [7:0] v_sync_fall_column;
    } expected_timing_t;

    localparam expected_timing_t EXPECTED_9IN = {
        16'd24, 16'd64, 16'd16, 16'd40, 16'd1280, 16'd16640, 16'd48,
        7'd28, 5'd0, 8'd0, 7'd30, 5'd4, 8'd0
    };

    localparam expected_timing_t EXPECTED_12IN = {
        16'd15, 16'd50, 16'd9, 16'd40, 16'd800, 16'd16650, 16'd0,
        7'd29, 5'd0, 8'd0, 7'd30, 5'd6, 8'd0
    };

    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic clk16_en;
    logic clk8_en;
    logic crtc_clk_en;

    timing timing (
        .sys_clock_i(clock),
        .clk16_en_o(clk16_en),
        .clk8_en_o(clk8_en),
        .cpu_be_o(),
        .cpu_clock_o(),
        .soft_cpu_clock_o(),
        .cpu_addr_strobe_o(),
        .cpu_data_strobe_o(crtc_clk_en),
        .cpu_data_en_o(),
        .cpu_hold_strobe_o(),
        .cpu_wr_en_o(),
        .load_sr1_o(),
        .load_sr2_o(),
        .grant_o(),
        .grant_valid_o()
    );

    logic reset;
    logic h_sync_9in;
    logic v_sync_9in;
    logic de_9in;
    logic h_sync_12in;
    logic v_sync_12in;
    logic de_12in;

    video_crtc video_crtc_9in (
        .wb_clock_i(clock),
        .wbp_addr_i(),
        .wbp_data_i(),
        .wbp_data_o(),
        .wbp_we_i(),
        .wbp_cycle_i(),
        .wbp_strobe_i(),
        .wbp_stall_o(),
        .wbp_ack_o(),
        .wbp_sel_i(),
        .reset_i(reset),
        .clk_en_i(crtc_clk_en),
        .cs_i(),
        .we_i(),
        .rs_i(),
        .data_i(),
        .data_o(),
        .data_oe(),
        .config_crt_i(1'b1),
        .h_sync_o(h_sync_9in),
        .v_sync_o(v_sync_9in),
        .de_o(de_9in),
        .ma_o(),
        .ra_o()
    );

    video_crtc video_crtc_12in (
        .wb_clock_i(clock),
        .wbp_addr_i(),
        .wbp_data_i(),
        .wbp_data_o(),
        .wbp_we_i(),
        .wbp_cycle_i(),
        .wbp_strobe_i(),
        .wbp_stall_o(),
        .wbp_ack_o(),
        .wbp_sel_i(),
        .reset_i(reset),
        .clk_en_i(crtc_clk_en),
        .cs_i(),
        .we_i(),
        .rs_i(),
        .data_i(),
        .data_o(),
        .data_oe(),
        .config_crt_i(1'b0),
        .h_sync_o(h_sync_12in),
        .v_sync_o(v_sync_12in),
        .de_o(de_12in),
        .ma_o(),
        .ra_o()
    );

    task automatic measure_h_sync(
        ref logic h_sync,
        input expected_timing_t expected
    );
        time first_rise;

        @(posedge h_sync);
        first_rise = $time;
        @(negedge h_sync);
        `assert_equal($time - first_rise, expected.h_sync_width_us * US);
        @(posedge h_sync);
        `assert_equal($time - first_rise, expected.h_sync_period_us * US);
    endtask

    task automatic measure_video_data_timing(
        ref logic h_sync,
        ref logic de,
        input expected_timing_t expected
    );
        time h_sync_rise;
        time de_rise;

        @(posedge h_sync);
        h_sync_rise = $time;
        @(posedge de);
        de_rise = $time;
        `assert_equal(de_rise - h_sync_rise, expected.h_sync_to_de_us * US);
        @(negedge de);
        `assert_equal($time - de_rise, expected.de_width_us * US);
    endtask

    task automatic measure_v_sync(
        ref logic v_sync,
        input expected_timing_t expected
    );
        time first_rise;

        @(posedge v_sync);
        first_rise = $time;
        @(negedge v_sync);
        `assert_equal($time - first_rise, expected.v_sync_width_us * US);
        @(posedge v_sync);
        `assert_equal($time - first_rise, expected.v_sync_period_us * US);
    endtask

    task automatic measure_v_sync_phase(
        ref logic       h_sync,
        ref logic       v_sync,
        ref logic [6:0] v_total_counter,
        ref logic [4:0] line_counter,
        ref logic [7:0] h_total_counter,
        input expected_timing_t expected
    );
        time v_sync_edge;

        @(posedge v_sync);
        v_sync_edge = $time;
        `assert_equal(v_total_counter, expected.v_sync_rise_row);
        `assert_equal(line_counter, expected.v_sync_rise_line);
        `assert_equal(h_total_counter, expected.v_sync_rise_column);
        if (expected.v_sync_to_h_sync_us != '0) begin
            @(posedge h_sync);
            `assert_equal($time - v_sync_edge, expected.v_sync_to_h_sync_us * US);
        end

        @(negedge v_sync);
        v_sync_edge = $time;
        `assert_equal(v_total_counter, expected.v_sync_fall_row);
        `assert_equal(line_counter, expected.v_sync_fall_line);
        `assert_equal(h_total_counter, expected.v_sync_fall_column);
        if (expected.v_sync_to_h_sync_us != '0) begin
            @(posedge h_sync);
            `assert_equal($time - v_sync_edge, expected.v_sync_to_h_sync_us * US);
        end
    endtask

    task run;
        reset = 1'b1;
        repeat (2) @(posedge crtc_clk_en);
        reset = 1'b0;

        fork
            // Verify the 9-inch CRTC timing against Thomas Skibo's measurements:
            // https://github.com/skibo/attiny2313_petvid
            measure_h_sync(h_sync_9in, EXPECTED_9IN);
            measure_video_data_timing(h_sync_9in, de_9in, EXPECTED_9IN);
            measure_v_sync(v_sync_9in, EXPECTED_9IN);
            measure_v_sync_phase(
                h_sync_9in,
                v_sync_9in,
                video_crtc_9in.v_total_counter,
                video_crtc_9in.line_counter,
                video_crtc_9in.h_total_counter,
                EXPECTED_9IN
            );

            // Verify the North American 12-inch CRTC power-on timing.
            measure_h_sync(h_sync_12in, EXPECTED_12IN);
            measure_video_data_timing(h_sync_12in, de_12in, EXPECTED_12IN);
            measure_v_sync(v_sync_12in, EXPECTED_12IN);
            measure_v_sync_phase(
                h_sync_12in,
                v_sync_12in,
                video_crtc_12in.v_total_counter,
                video_crtc_12in.line_counter,
                video_crtc_12in.h_total_counter,
                EXPECTED_12IN
            );
        join
    endtask

    `TB_INIT
endmodule
