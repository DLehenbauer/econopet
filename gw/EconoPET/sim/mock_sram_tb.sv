// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

//
// Testbench for mock_sram: exercises read and write cycles against the async
// SRAM model, verifying data correctness, high-Z behavior, and timing
// violation detection.
//

`include "./sim/tb.svh"

import common_pkg::*;

module mock_sram_tb;
    localparam int AW = RAM_ADDR_WIDTH;
    localparam int DW = DATA_WIDTH;

    // Timing parameters sourced from common_pkg (AS6C1008-55PCN defaults).
    // Local aliases keep the test code concise.
    localparam int tAA  = SRAM_tAA;
    localparam int tOE  = SRAM_tOE;
    localparam int tOHZ = SRAM_tOHZ;
    localparam int tCHZ = SRAM_tCHZ;
    localparam int tOLZ = SRAM_tOLZ;
    localparam int tCLZ = SRAM_tCLZ;
    localparam int tOH  = SRAM_tOH;
    localparam int tWP  = SRAM_tWP;
    localparam int tAW  = SRAM_tAW;
    localparam int tDW  = SRAM_tDW;
    localparam int tACE = SRAM_tACE;

    // -------------------------------------------------------------------------
    // DUT signals
    // -------------------------------------------------------------------------
    logic [AW-1:0] addr;
    wire  [DW-1:0] data;          // bidirectional bus
    logic [DW-1:0] data_drv;      // testbench driver value
    logic          data_drv_en;   // testbench drives bus when 1

    logic ce_n;
    logic oe_n;
    logic we_n;

    assign data = data_drv_en ? data_drv : {DW{1'bz}};

    // -------------------------------------------------------------------------
    // DUT instantiation
    // -------------------------------------------------------------------------
    mock_sram #(
        .ADDR_WIDTH (AW),
        .DW         (DW)
    ) dut (
        .addr_i  (addr),
        .data_io (data),
        .ce_ni   (ce_n),
        .oe_ni   (oe_n),
        .we_ni   (we_n)
    );

    // -------------------------------------------------------------------------
    // Helper tasks
    // -------------------------------------------------------------------------

    // Perform a write cycle with proper timing.
    //   1. Drive address and assert CE
    //   2. Assert WE (start write pulse)
    //   3. Drive data onto bus (satisfying tDW before WE rises)
    //   4. Deassert WE (commits write)
    //   5. Release bus and deassert CE
    task automatic do_write(
        input logic [AW-1:0] w_addr,
        input logic [DW-1:0] w_data
    );
        // Setup: drive address, assert CE, deassert OE
        addr       = w_addr;
        oe_n       = 1'b1;
        ce_n       = 1'b0;
        data_drv_en = 1'b0;

        // Address setup before WE falls (must satisfy tAW: address valid tAW
        // before WE rises, so we need at least tAW - tWP here)
        #(tAW > tWP ? tAW - tWP + 1 : 1);

        // Assert WE (begin write pulse)
        we_n = 1'b0;

        // Drive data after WE falls (but well before WE rises to satisfy tDW)
        #(tWP - tDW);
        data_drv    = w_data;
        data_drv_en = 1'b1;

        // Wait remaining time to satisfy tWP
        #(tDW);

        // Deassert WE (end of write, data is latched)
        we_n = 1'b1;

        // Small hold time
        #1;

        // Release bus
        data_drv_en = 1'b0;

        // Deassert CE
        ce_n = 1'b1;

        #1;
    endtask

    // Perform a read cycle and return data.
    //   1. Drive address and assert CE + OE
    //   2. Wait tAA for data to become valid
    //   3. Sample data
    //   4. Deassert OE and CE
    task automatic do_read(
        input  logic [AW-1:0] r_addr,
        output logic [DW-1:0] r_data
    );
        // Drive address
        addr = r_addr;
        we_n = 1'b1;
        ce_n = 1'b0;

        // Assert OE
        oe_n = 1'b0;

        // Wait for address access time
        #(tAA + 1);

        // Sample data
        r_data = data;

        // Deassert OE
        oe_n = 1'b1;

        // Wait for outputs to go high-Z
        #(tOHZ + 1);

        // Deassert CE
        ce_n = 1'b1;

        #1;
    endtask

    // -------------------------------------------------------------------------
    // Test: write then read back
    // -------------------------------------------------------------------------
    task test_write_read;
        logic [DW-1:0] rd;

        $display("[%t] TEST: write then read back", $time);

        do_write(17'h00000, 8'hA5);
        do_read (17'h00000, rd);
        `assert_equal(rd, 8'hA5);

        do_write(17'h1FFFF, 8'h3C);
        do_read (17'h1FFFF, rd);
        `assert_equal(rd, 8'h3C);

        // Write multiple, then read all back
        do_write(17'h00100, 8'h11);
        do_write(17'h00101, 8'h22);
        do_write(17'h00102, 8'h33);

        do_read(17'h00100, rd); `assert_equal(rd, 8'h11);
        do_read(17'h00101, rd); `assert_equal(rd, 8'h22);
        do_read(17'h00102, rd); `assert_equal(rd, 8'h33);

        $display("[%t] PASS: write then read back", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: overwrite and verify
    // -------------------------------------------------------------------------
    task test_overwrite;
        logic [DW-1:0] rd;

        $display("[%t] TEST: overwrite and verify", $time);

        do_write(17'h00200, 8'hFF);
        do_read (17'h00200, rd);
        `assert_equal(rd, 8'hFF);

        // Overwrite with new value
        do_write(17'h00200, 8'h42);
        do_read (17'h00200, rd);
        `assert_equal(rd, 8'h42);

        $display("[%t] PASS: overwrite and verify", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: high-Z when deselected
    // -------------------------------------------------------------------------
    task test_highz_deselected;
        $display("[%t] TEST: high-Z when deselected", $time);

        // CE and OE both deasserted: bus should be high-Z
        ce_n        = 1'b1;
        oe_n        = 1'b1;
        we_n        = 1'b1;
        data_drv_en = 1'b0;

        #(tOHZ + tCHZ + 1);
        `assert_exact_equal(data, 8'hzz);

        $display("[%t] PASS: high-Z when deselected", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: high-Z when OE deasserted during read
    // -------------------------------------------------------------------------
    task test_highz_oe_deasserted;
        logic [DW-1:0] rd;

        $display("[%t] TEST: high-Z when OE deasserted", $time);

        // Write a known value
        do_write(17'h00300, 8'hBB);

        // Start reading (CE asserted, OE asserted)
        addr = 17'h00300;
        we_n = 1'b1;
        ce_n = 1'b0;
        oe_n = 1'b0;
        data_drv_en = 1'b0;

        #(tAA + 1);
        rd = data;
        `assert_equal(rd, 8'hBB);

        // Deassert OE: output becomes indeterminate, then high-Z after tOHZ.
        oe_n = 1'b1;
        #1;
        `assert_exact_equal(data, 8'hxx);

        #(tOHZ);
        `assert_exact_equal(data, 8'hzz);

        ce_n = 1'b1;
        #1;

        $display("[%t] PASS: high-Z when OE deasserted", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: bus sharing (simulate controller handoff)
    // -------------------------------------------------------------------------
    task test_bus_sharing;
        logic [DW-1:0] rd;

        $display("[%t] TEST: bus sharing (controller handoff)", $time);

        // Controller A writes a value
        do_write(17'h00400, 8'hDE);

        // SRAM is deselected (high-Z) while another device uses the bus
        ce_n        = 1'b1;
        oe_n        = 1'b1;
        we_n        = 1'b1;
        data_drv_en = 1'b0;
        #(tOHZ + 1);
        `assert_exact_equal(data, 8'hzz);

        // An external device drives the bus (e.g., CPU talking to I/O)
        data_drv    = 8'h99;
        data_drv_en = 1'b1;
        #20;
        `assert_equal(data, 8'h99);

        // External device releases the bus
        data_drv_en = 1'b0;
        #5;

        // Controller B reads the value from SRAM
        do_read(17'h00400, rd);
        `assert_equal(rd, 8'hDE);

        $display("[%t] PASS: bus sharing (controller handoff)", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: read address change while OE is asserted (output updates after tAA)
    // -------------------------------------------------------------------------
    task test_read_addr_change;
        logic [DW-1:0] rd;

        $display("[%t] TEST: read address change while active", $time);

        // Preload two addresses
        do_write(17'h00500, 8'hAA);
        do_write(17'h00501, 8'h55);

        // Begin read of first address
        addr = 17'h00500;
        we_n = 1'b1;
        ce_n = 1'b0;
        oe_n = 1'b0;
        data_drv_en = 1'b0;

        #(tAA + 1);
        rd = data;
        `assert_equal(rd, 8'hAA);

        // Change address (output should update after tAA)
        addr = 17'h00501;
        #(tAA + 1);
        rd = data;
        `assert_equal(rd, 8'h55);

        // Done
        oe_n = 1'b1;
        #(tOHZ + 1);
        ce_n = 1'b1;
        #1;

        $display("[%t] PASS: read address change while active", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: indeterminate output between tOH and tAA on address change
    // -------------------------------------------------------------------------
    task test_indeterminate_window;
        logic [DW-1:0] rd;

        $display("[%t] TEST: indeterminate window (tOH to tAA)", $time);

        // Preload two addresses with distinct values
        do_write(17'h00A00, 8'hAA);
        do_write(17'h00A01, 8'h55);

        // Begin read of first address
        addr = 17'h00A00;
        we_n = 1'b1;
        ce_n = 1'b0;
        oe_n = 1'b0;
        data_drv_en = 1'b0;

        #(tAA + 1);
        rd = data;
        `assert_equal(rd, 8'hAA);

        // Change address: old data should be held for tOH
        addr = 17'h00A01;

        // Just before tOH expires, old data should still be valid
        #(tOH - 1);
        rd = data;
        `assert_equal(rd, 8'hAA);

        // After tOH, output should be indeterminate ('x)
        #2;
        rd = data;
        `assert_exact_equal(rd, 8'hxx);

        // After tAA, new data should be valid
        #(tAA - tOH);
        rd = data;
        `assert_equal(rd, 8'h55);

        // Cleanup
        oe_n = 1'b1;
        #(tOHZ + 1);
        ce_n = 1'b1;
        #1;

        $display("[%t] PASS: indeterminate window (tOH to tAA)", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: OE enable transition ('x between low-Z and valid data)
    // -------------------------------------------------------------------------
    task test_oe_enable_transition;
        logic [DW-1:0] rd;

        $display("[%t] TEST: OE enable transition", $time);

        // Preload
        do_write(17'h00B00, 8'hDD);

        // Set address, CE asserted, OE deasserted
        addr = 17'h00B00;
        we_n = 1'b1;
        ce_n = 1'b0;
        oe_n = 1'b1;
        data_drv_en = 1'b0;

        #5;

        // Assert OE: output goes low-Z at max(tOLZ, tCLZ), then valid at tOE.
        oe_n = 1'b0;

        // Just after tCLZ (max of tOLZ=5, tCLZ=10), output is driven but
        // indeterminate.
        #(tCLZ + 1);
        rd = data;
        `assert_exact_equal(rd, 8'hxx);

        // After tOE total from OE assertion, data should be valid.
        #(tOE - tCLZ);
        rd = data;
        `assert_equal(rd, 8'hDD);

        // Cleanup
        oe_n = 1'b1;
        #(tOHZ + 1);
        ce_n = 1'b1;
        #1;

        $display("[%t] PASS: OE enable transition", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: CE access time (tACE) with indeterminate window
    // -------------------------------------------------------------------------
    task test_ce_access_time;
        logic [DW-1:0] rd;

        $display("[%t] TEST: CE access time", $time);

        // Preload
        do_write(17'h00600, 8'hCC);

        // Set address and OE first, then assert CE
        addr = 17'h00600;
        we_n = 1'b1;
        oe_n = 1'b0;
        data_drv_en = 1'b0;

        #5;

        // Assert CE: output goes low-Z at max(tOLZ, tCLZ), indeterminate
        // until tACE.
        ce_n = 1'b0;

        #(tCLZ + 1);
        rd = data;
        `assert_exact_equal(rd, 8'hxx);

        // Wait for CE access time
        #(tACE - tCLZ);

        rd = data;
        `assert_equal(rd, 8'hCC);

        // Cleanup
        oe_n = 1'b1;
        #(tOHZ + 1);
        ce_n = 1'b1;
        #1;

        $display("[%t] PASS: CE access time", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: fill utility
    // -------------------------------------------------------------------------
    task test_fill;
        logic [DW-1:0] rd;

        $display("[%t] TEST: fill utility", $time);

        dut.fill(17'h00700, 17'h00703, 8'hEE);

        do_read(17'h00700, rd); `assert_equal(rd, 8'hEE);
        do_read(17'h00701, rd); `assert_equal(rd, 8'hEE);
        do_read(17'h00702, rd); `assert_equal(rd, 8'hEE);
        do_read(17'h00703, rd); `assert_equal(rd, 8'hEE);

        $display("[%t] PASS: fill utility", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: WE does not commit write when CE is deasserted
    // -------------------------------------------------------------------------
    task test_no_write_without_ce;
        logic [DW-1:0] rd;

        $display("[%t] TEST: no write without CE", $time);

        // Write a known value first
        do_write(17'h00800, 8'h00);
        do_read (17'h00800, rd);
        `assert_equal(rd, 8'h00);

        // Attempt write with CE deasserted
        addr        = 17'h00800;
        ce_n        = 1'b1;       // CE not asserted
        oe_n        = 1'b1;
        we_n        = 1'b0;
        data_drv    = 8'hFF;
        data_drv_en = 1'b1;

        #(tWP + 1);
        we_n = 1'b1;
        #1;
        data_drv_en = 1'b0;

        // Original value should be unchanged
        do_read(17'h00800, rd);
        `assert_equal(rd, 8'h00);

        $display("[%t] PASS: no write without CE", $time);
    endtask

    // -------------------------------------------------------------------------
    // Test: rapid bus turnaround (deselect, external driver, reselect)
    // -------------------------------------------------------------------------
    task test_rapid_turnaround;
        logic [DW-1:0] rd;

        $display("[%t] TEST: rapid bus turnaround", $time);

        do_write(17'h00900, 8'h77);

        // Read to verify
        do_read(17'h00900, rd);
        `assert_equal(rd, 8'h77);

        // Deselect SRAM
        ce_n = 1'b1;
        oe_n = 1'b1;
        we_n = 1'b1;
        #(tOHZ + 1);

        // External device briefly drives bus
        data_drv    = 8'hAA;
        data_drv_en = 1'b1;
        #10;
        data_drv_en = 1'b0;
        #1;

        // Re-read from SRAM
        do_read(17'h00900, rd);
        `assert_equal(rd, 8'h77);

        $display("[%t] PASS: rapid bus turnaround", $time);
    endtask

    // -------------------------------------------------------------------------
    // Main test sequence
    // -------------------------------------------------------------------------
    task run;
        // Initialize signals to idle state
        addr        = '0;
        ce_n        = 1'b1;
        oe_n        = 1'b1;
        we_n        = 1'b1;
        data_drv    = '0;
        data_drv_en = 1'b0;

        #10;

        test_write_read;
        test_overwrite;
        test_highz_deselected;
        test_highz_oe_deasserted;
        test_bus_sharing;
        test_read_addr_change;
        test_indeterminate_window;
        test_oe_enable_transition;
        test_ce_access_time;
        test_fill;
        test_no_write_without_ce;
        test_rapid_turnaround;

        $display("[%t] All mock_sram tests passed.", $time);
    endtask

    `TB_INIT
endmodule
