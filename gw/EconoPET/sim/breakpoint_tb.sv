// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

module breakpoint_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    // DUT signals
    logic                      cpu_sync;
    logic [   DATA_WIDTH-1:0]  cpu_data;
    logic [CPU_ADDR_WIDTH-1:0] cpu_addr;
    logic                      cpu_data_strobe;
    logic                      cpu_be;
    logic                      cpu_ready_in;
    logic                      clear;

    logic                      cpu_ready_out;
    logic                      halted;
    logic [CPU_ADDR_WIDTH-1:0] bp_addr;

    breakpoint dut (
        .sys_clock_i(clock),
        .cpu_sync_i(cpu_sync),
        .cpu_data_i(cpu_data),
        .cpu_addr_i(cpu_addr),
        .cpu_data_strobe_i(cpu_data_strobe),
        .cpu_be_i(cpu_be),
        .cpu_ready_i(cpu_ready_in),
        .clear_i(clear),
        .cpu_ready_o(cpu_ready_out),
        .halted_o(halted),
        .addr_o(bp_addr)
    );

    // Helper: pulse cpu_data_strobe for one clock cycle with given bus state.
    // Changes signals on the falling edge to guarantee setup time before the
    // rising edge where the DUT's always_ff samples them.
    task strobe(
        input logic [DATA_WIDTH-1:0] data,
        input logic [CPU_ADDR_WIDTH-1:0] addr,
        input logic sync,
        input logic be
    );
        @(negedge clock);
        cpu_data        = data;
        cpu_addr        = addr;
        cpu_sync        = sync;
        cpu_be          = be;
        cpu_data_strobe = 1'b1;
        @(negedge clock);
        cpu_data_strobe = 1'b0;
        cpu_sync        = 1'b0;
        cpu_be          = 1'b0;
        @(posedge clock);
    endtask

    task test_no_halt_on_normal_opcode;
        $display("[%t] test_no_halt_on_normal_opcode", $time);

        // Fetch a non-STP opcode (LDA #imm = $A9) during opcode fetch (SYNC=1, BE=1)
        strobe(8'hA9, 16'h0400, /* sync */ 1'b1, /* be */ 1'b1);

        `assert_equal(halted, 1'b0)
        `assert_equal(cpu_ready_out, 1'b1)
    endtask

    task test_halt_on_stp_opcode;
        $display("[%t] test_halt_on_stp_opcode", $time);

        // Fetch STP opcode ($DB) during opcode fetch (SYNC=1, BE=1)
        strobe(8'hDB, 16'h1234, /* sync */ 1'b1, /* be */ 1'b1);

        `assert_equal(halted, 1'b1)
        `assert_equal(cpu_ready_out, 1'b0)
        `assert_equal(bp_addr, 16'h1234)
    endtask

    // Helper: pulse clear for one clock cycle using negedge-driven timing.
    task pulse_clear;
        @(negedge clock);
        clear = 1'b1;
        @(negedge clock);
        clear = 1'b0;
        @(posedge clock);
    endtask

    task test_clear_resumes;
        $display("[%t] test_clear_resumes", $time);

        // Pulse clear to resume
        pulse_clear;

        `assert_equal(halted, 1'b0)
        `assert_equal(cpu_ready_out, 1'b1)
    endtask

    task test_no_halt_when_sync_low;
        $display("[%t] test_no_halt_when_sync_low", $time);

        // STP byte on data bus but SYNC=0 (operand fetch, not opcode)
        strobe(8'hDB, 16'h5678, /* sync */ 1'b0, /* be */ 1'b1);

        `assert_equal(halted, 1'b0)
        `assert_equal(cpu_ready_out, 1'b1)
    endtask

    task test_no_halt_when_be_low;
        $display("[%t] test_no_halt_when_be_low", $time);

        // STP byte on data bus with SYNC=1 but BE=0 (CPU does not own bus)
        strobe(8'hDB, 16'h9ABC, /* sync */ 1'b1, /* be */ 1'b0);

        `assert_equal(halted, 1'b0)
        `assert_equal(cpu_ready_out, 1'b1)
    endtask

    task test_ready_passthrough;
        $display("[%t] test_ready_passthrough", $time);

        // When MCU deasserts ready, output should be low even without a halt
        @(negedge clock);
        cpu_ready_in = 1'b0;
        @(posedge clock);

        `assert_equal(halted, 1'b0)
        `assert_equal(cpu_ready_out, 1'b0)

        @(negedge clock);
        cpu_ready_in = 1'b1;
        @(posedge clock);

        `assert_equal(cpu_ready_out, 1'b1)
    endtask

    task test_addr_captured_correctly;
        $display("[%t] test_addr_captured_correctly", $time);

        // Hit breakpoint at a specific address
        strobe(8'hDB, 16'hBEEF, /* sync */ 1'b1, /* be */ 1'b1);

        `assert_equal(halted, 1'b1)
        `assert_equal(bp_addr, 16'hBEEF)

        // Clear and hit at a different address
        pulse_clear;

        strobe(8'hDB, 16'hCAFE, /* sync */ 1'b1, /* be */ 1'b1);

        `assert_equal(halted, 1'b1)
        `assert_equal(bp_addr, 16'hCAFE)

        // Clean up
        pulse_clear;
    endtask

    task test_halt_persists_without_clear;
        $display("[%t] test_halt_persists_without_clear", $time);

        // Trigger halt
        strobe(8'hDB, 16'h0100, /* sync */ 1'b1, /* be */ 1'b1);

        `assert_equal(halted, 1'b1)

        // Wait several cycles without clearing
        repeat (10) @(posedge clock);

        `assert_equal(halted, 1'b1)
        `assert_equal(cpu_ready_out, 1'b0)

        // Clean up
        pulse_clear;
    endtask

    task run;
        // Initialize signals
        cpu_sync        = 1'b0;
        cpu_data        = 8'h00;
        cpu_addr        = 16'h0000;
        cpu_data_strobe = 1'b0;
        cpu_be          = 1'b0;
        cpu_ready_in    = 1'b1;
        clear           = 1'b0;

        @(posedge clock);

        test_no_halt_on_normal_opcode;
        test_halt_on_stp_opcode;
        test_clear_resumes;
        test_no_halt_when_sync_low;
        test_no_halt_when_be_low;
        test_ready_passthrough;
        test_addr_captured_correctly;
        test_halt_persists_without_clear;
    endtask

    `TB_INIT
endmodule
