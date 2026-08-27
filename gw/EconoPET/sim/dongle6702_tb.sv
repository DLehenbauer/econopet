// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Unit bench for the 6702 dongle. The boot benches cannot reach it: the
// challenge/response ships with the Waterloo languages, which load from disk
// into the banked $9000 window -- one copy per language, and the code builds
// the $EFE0 pointer arithmetically rather than storing it. The $A000-$FFFF
// OS/monitor set never touches $EFE0-$EFE3, so reaching the power-on menu
// does not exercise this chip at all.
//
// The expected values below (MAGIC, the $C6 intermediate and GOLDEN_VAL) were
// reproduced independently from VICE's petmem.c dongle6702 reference, so they
// check the algorithm rather than this implementation against itself.
module dongle6702_tb;
    logic                      clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    localparam logic [7:0] MAGIC      = 8'hD6;   // dongle6702.sv power-on value
    localparam logic [7:0] GOLDEN_VAL = 8'h91;   // VICE: val after write_ramp()

    logic                      reset  = 1'b0;
    logic                      cpu_be = 1'b1;
    logic                      strobe = 1'b0;
    logic                      we     = 1'b0;
    logic                      enable = 1'b1;
    logic [CPU_ADDR_WIDTH-1:0] addr   = 16'hEFE0;
    logic [    DATA_WIDTH-1:0] data_i = '0;
    logic [    DATA_WIDTH-1:0] data_o;
    logic                      data_oe;

    dongle6702 dut (
        .sys_clock_i(clock),
        .reset_i(reset),
        .cpu_be_i(cpu_be),
        .cpu_data_strobe_i(strobe),
        .cpu_addr_i(addr),
        .cpu_data_i(data_i),
        .cpu_data_o(data_o),
        .cpu_data_oe(data_oe),
        .cpu_we_i(we),
        .enable_i(enable)
    );

    // Stimulus changes on negedge so the DUT's posedge logic sees stable inputs.
    task automatic do_reset;
        @(negedge clock);
        reset = 1'b1;
        @(negedge clock);
        reset = 1'b0;
    endtask

    task automatic do_write(input logic [15:0] a, input logic [7:0] d);
        @(negedge clock);
        addr = a; data_i = d; we = 1'b1; strobe = 1'b1;
        @(negedge clock);
        strobe = 1'b0; we = 1'b0;
    endtask

    // Reads are registered, so the value lands one clock after the address.
    task automatic do_read(input logic [15:0] a, output logic [7:0] d, output logic oe);
        @(negedge clock);
        addr = a; we = 1'b0;
        @(negedge clock);
        d = data_o; oe = data_oe;
    endtask

    task automatic check_read(input string name, input logic [15:0] a,
                              input logic [7:0] expected, input logic expected_oe);
        logic [7:0] d;
        logic       oe;
        do_read(a, d, oe);
        $display("[%t]   %-28s $%04x -> oe=%b data=$%02x", $time, name, a, oe, d);
        `assert_equal(oe, expected_oe)
        if (expected_oe) `assert_equal(d, expected)
    endtask

    // Alternating even/odd values: each even write arms the state machine and
    // the odd write that follows mutates it, so this lands 8 mutations.
    task automatic write_ramp;
        for (int i = 0; i < 16; i++) do_write(16'hEFE0, 8'(i));
    endtask

    task run;
        logic [7:0] d, d2;
        logic       oe;

        $display("[%t] BEGIN dongle6702 unit test", $time);
        do_reset;

        // Power-on value, and the $EFE0-$EFE3 partial decode (74LS08 on the
        // board): all four addresses are the same register.
        check_read("reset value", 16'hEFE0, MAGIC, 1'b1);
        check_read("mirror $EFE1", 16'hEFE1, MAGIC, 1'b1);
        check_read("mirror $EFE2", 16'hEFE2, MAGIC, 1'b1);
        check_read("mirror $EFE3", 16'hEFE3, MAGIC, 1'b1);

        // Outside the window, and the two ways the dongle is hidden.
        check_read("outside window $EFE4", 16'hEFE4, 8'h00, 1'b0);
        check_read("outside window $EFDF", 16'hEFDF, 8'h00, 1'b0);
        enable = 1'b0;
        check_read("enable_i low (MMU flat)", 16'hEFE0, 8'h00, 1'b0);
        enable = 1'b1;
        cpu_be = 1'b0;
        check_read("cpu_be_i low", 16'hEFE0, 8'h00, 1'b0);
        cpu_be = 1'b1;

        // Reads must not disturb the state machine.
        do_read(16'hEFE0, d, oe);
        do_read(16'hEFE0, d2, oe);
        `assert_equal(d2, d)

        // Parity gating. After reset the machine wants an even write, so an odd
        // one is dropped entirely; the even write that follows arms it without
        // mutating val; only the next odd write changes anything.
        do_reset;
        do_write(16'hEFE0, 8'h01);          // wrong parity -- ignored
        check_read("after odd write (ignored)", 16'hEFE0, MAGIC, 1'b1);
        do_write(16'hEFE0, 8'h02);          // right parity, arms only
        check_read("after even write (armed)", 16'hEFE0, MAGIC, 1'b1);
        do_write(16'hEFE0, 8'h03);          // mutates
        do_read(16'hEFE0, d, oe);
        $display("[%t]   after odd write: $%02x", $time, d);
        assert (d != MAGIC) else $fatal(1, "odd write did not mutate val");

        // A write while deselected must be ignored.
        do_reset;
        do_write(16'hEFE4, 8'h00);
        do_write(16'hEFE4, 8'h01);
        check_read("write outside window", 16'hEFE0, MAGIC, 1'b1);

        // Reset restores the power-on value after the state has moved.
        do_reset;
        write_ramp;
        do_read(16'hEFE0, d, oe);
        $display("[%t]   val after ramp = $%02x (expect $%02x)", $time, d, GOLDEN_VAL);
        `assert_equal(d, GOLDEN_VAL)
        do_reset;
        check_read("reset after mutation", 16'hEFE0, MAGIC, 1'b1);

        $display("[%t] END dongle6702 unit test", $time);
    endtask

    `TB_INIT
endmodule
