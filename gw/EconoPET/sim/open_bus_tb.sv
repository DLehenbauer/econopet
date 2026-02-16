// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

module open_bus_tb;
    logic sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(sys_clock));
    initial clock_gen.start;

    logic                  cpu_data_strobe = '0;
    logic [DATA_WIDTH-1:0] cpu_data_i      = 'x;
    logic                  unmapped        = '0;
    logic                  cpu_be          = '0;
    logic                  cpu_we          = '0;

    logic [DATA_WIDTH-1:0] data_o;
    logic                  data_oe;

    open_bus dut (
        .sys_clock_i(sys_clock),
        .cpu_data_strobe_i(cpu_data_strobe),
        .cpu_data_i(cpu_data_i),
        .unmapped_i(unmapped),
        .cpu_be_i(cpu_be),
        .cpu_we_i(cpu_we),
        .data_o(data_o),
        .data_oe(data_oe)
    );

    // Simulate a completed bus cycle: present data and pulse strobe for one clock.
    task static bus_cycle(input logic [DATA_WIDTH-1:0] data);
        @(negedge sys_clock);
        cpu_data_i = data;
        cpu_data_strobe = 1'b1;
        @(negedge sys_clock);
        cpu_data_strobe = 1'b0;
    endtask

    task run;
        // ---- OE is deasserted when no unmapped read is active ----
        @(negedge sys_clock);
        `assert_equal(data_oe, 1'b0);

        // OE stays low during writes to unmapped addresses.
        unmapped = 1'b1; cpu_be = 1'b1; cpu_we = 1'b1;
        @(negedge sys_clock);
        `assert_equal(data_oe, 1'b0);

        // OE stays low when CPU is not driving the bus.
        unmapped = 1'b1; cpu_be = 1'b0; cpu_we = 1'b0;
        @(negedge sys_clock);
        `assert_equal(data_oe, 1'b0);

        // OE stays low when address is mapped.
        unmapped = 1'b0; cpu_be = 1'b1; cpu_we = 1'b0;
        @(negedge sys_clock);
        `assert_equal(data_oe, 1'b0);

        // ---- Captured data is returned on unmapped reads ----

        // Simulate a bus cycle that transfers $A5.
        bus_cycle(8'hA5);

        // Begin an unmapped read -- should output $A5.
        unmapped = 1'b1; cpu_be = 1'b1; cpu_we = 1'b0;
        @(negedge sys_clock);
        `assert_equal(data_oe, 1'b1);
        `assert_equal(data_o, 8'hA5);

        // ---- Captured value updates with each bus cycle ----

        bus_cycle(8'h42);
        unmapped = 1'b1; cpu_be = 1'b1; cpu_we = 1'b0;
        @(negedge sys_clock);
        `assert_equal(data_oe, 1'b1);
        `assert_equal(data_o, 8'h42);

        bus_cycle(8'hFF);
        unmapped = 1'b1; cpu_be = 1'b1; cpu_we = 1'b0;
        @(negedge sys_clock);
        `assert_equal(data_o, 8'hFF);

        // ---- Strobe required to latch new data ----

        // Present new data without strobe -- captured value should not change.
        @(negedge sys_clock);
        cpu_data_i = 8'h00;
        @(negedge sys_clock);
        @(negedge sys_clock);
        unmapped = 1'b1; cpu_be = 1'b1; cpu_we = 1'b0;
        @(negedge sys_clock);
        `assert_equal(data_o, 8'hFF);  // Still $FF from previous strobe

    endtask

    `TB_INIT
endmodule
