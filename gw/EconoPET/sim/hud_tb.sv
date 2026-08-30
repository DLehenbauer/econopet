// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Unit test for the HUD overlay (hud.sv), which substitutes its characters over
// the video controller's VRAM reads. A wb_driver plays the RP2040 (writing the
// text buffer + control regs over the peripheral port); the test then presents
// video read addresses on vid_addr_i and checks ov_active_o/ov_char_o.
module hud_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    // wb_driver -> HUD peripheral
    logic [WB_ADDR_WIDTH-1:0] p_addr;
    logic [   DATA_WIDTH-1:0] p_poci, p_pico;
    logic                     p_we, p_cycle, p_strobe, p_stall, p_ack;

    // Scan-out substitution port
    logic [WB_ADDR_WIDTH-1:0] vid_addr;
    logic                     ov_active;
    logic [   DATA_WIDTH-1:0] ov_char;

    hud hud (
        .wb_clock_i(clock),
        .wbp_addr_i(p_addr),
        .wbp_data_i(p_pico),
        .wbp_data_o(p_poci),
        .wbp_we_i(p_we),
        .wbp_cycle_i(p_cycle),
        .wbp_strobe_i(p_strobe),
        .wbp_stall_o(p_stall),
        .wbp_ack_o(p_ack),
        .wbp_sel_i(1'b1),

        .vid_addr_i(vid_addr),
        .ov_active_o(ov_active),
        .ov_char_o(ov_char)
    );

    wb_driver wb (
        .wb_clock_i(clock),
        .wb_addr_o(p_addr),
        .wb_data_i(p_poci),
        .wb_data_o(p_pico),
        .wb_we_o(p_we),
        .wb_cycle_o(p_cycle),
        .wb_strobe_o(p_strobe),
        .wb_ack_i(p_ack),
        .wb_stall_i(p_stall)
    );

    task hud_wr_ctrl(input logic [3:0] regsel, input logic [DATA_WIDTH-1:0] data);
        wb.write(common_pkg::wb_hud_addr(HUD_CTRL_FLAG | 9'(regsel)), data);
    endtask

    task hud_wr_buf(input logic [7:0] idx, input logic [DATA_WIDTH-1:0] data);
        wb.write(common_pkg::wb_hud_addr(9'(idx)), data);
    endtask

    task set_placement(input logic [VRAM_ADDR_WIDTH-1:0] off, input logic [DATA_WIDTH-1:0] len);
        hud_wr_ctrl(HUD_REG_OFF_LO, off[7:0]);
        hud_wr_ctrl(HUD_REG_OFF_HI, {5'b0, off[VRAM_ADDR_WIDTH-1:8]});
        hud_wr_ctrl(HUD_REG_LEN, len);
    endtask

    // Present a VRAM read address for offset 'off', hold it stable a few cycles
    // (as the video controller does), and sample the substitution outputs.
    task probe(input logic [VRAM_ADDR_WIDTH-1:0] off, output logic act, output logic [DATA_WIDTH-1:0] ch);
        vid_addr = common_pkg::wb_vram_addr(off);
        repeat (3) @(posedge clock);
        #1;
        act = ov_active;
        ch  = ov_char;
    endtask

    task run;
        logic act;
        logic [DATA_WIDTH-1:0] ch;

        wb.reset;
        vid_addr = '0;
        @(posedge clock);

        // Load buf[i] = 0x40 + i, place the overlay at offset 984, length 5.
        for (int i = 0; i < 8; i++) hud_wr_buf(i[7:0], 8'h40 + i[7:0]);
        set_placement(11'd984, 8'd5);

        // Disabled: never substitute.
        hud_wr_ctrl(HUD_REG_CTRL, 8'h00);
        probe(11'd984, act, ch);
        `assert_equal(act, 1'b0);

        // Enabled: substitute exactly within [984, 989), returning buf[off-984].
        hud_wr_ctrl(HUD_REG_CTRL, 8'h01);

        probe(11'd983, act, ch);           // just before the run
        `assert_equal(act, 1'b0);

        for (int i = 0; i < 5; i++) begin
            probe(11'(984 + i), act, ch);
            `assert_equal(act, 1'b1);
            `assert_equal(ch, (8'h40 + i[7:0]));
        end

        probe(11'd989, act, ch);           // just past the run (984+5)
        `assert_equal(act, 1'b0);

        // A non-VRAM address (character ROM / BRAM) must never substitute, even
        // if its low bits fall in the offset range.
        vid_addr = common_pkg::wb_bram_addr(12'd984);
        repeat (3) @(posedge clock);
        #1;
        `assert_equal(ov_active, 1'b0);

        // len = 0 disables the run entirely.
        set_placement(11'd984, 8'd0);
        probe(11'd984, act, ch);
        `assert_equal(act, 1'b0);

        $display("[%t] hud_tb: all checks passed", $time);
    endtask

    `TB_INIT
endmodule
