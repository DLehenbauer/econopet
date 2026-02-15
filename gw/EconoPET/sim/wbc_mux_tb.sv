/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 *
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

`include "./sim/tb.svh"

import common_pkg::*;

module wbc_mux_tb;
    localparam COUNT = 2;

    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    // Controller inputs (directly driven by test)
    logic [COUNT-1:0]                    wbc_cycle;
    logic [COUNT-1:0]                    wbc_strobe;
    logic [COUNT-1:0][WB_ADDR_WIDTH-1:0] wbc_addr;
    logic [COUNT-1:0][   DATA_WIDTH-1:0] wbc_dout;
    logic [COUNT-1:0]                    wbc_we;

    // Controller outputs (from mux)
    logic [COUNT-1:0][DATA_WIDTH-1:0] wbc_din;
    logic [COUNT-1:0]                 wbc_stall;
    logic [COUNT-1:0]                 wbc_ack;

    // Bus outputs (from mux)
    logic                     wb_cycle;
    logic                     wb_strobe;
    logic [WB_ADDR_WIDTH-1:0] wb_addr;
    logic [   DATA_WIDTH-1:0] wb_dout;
    logic                     wb_we;

    // Bus inputs (directly driven by test, simulating peripheral response)
    logic [DATA_WIDTH-1:0] wb_din;
    logic                  wb_stall;
    logic                  wb_ack;

    // Control signals
    logic [$clog2(COUNT)-1:0] wbc_grant;
    logic                     wbc_grant_valid;

    wbc_mux #(
        .COUNT(COUNT)
    ) dut (
        .wb_clock_i(clock),

        // Controller side
        .wbc_cycle_i(wbc_cycle),
        .wbc_strobe_i(wbc_strobe),
        .wbc_addr_i(wbc_addr),
        .wbc_din_o(wbc_din),
        .wbc_dout_i(wbc_dout),
        .wbc_we_i(wbc_we),
        .wbc_stall_o(wbc_stall),
        .wbc_ack_o(wbc_ack),

        // Bus side
        .wb_cycle_o(wb_cycle),
        .wb_strobe_o(wb_strobe),
        .wb_addr_o(wb_addr),
        .wb_din_i(wb_din),
        .wb_dout_o(wb_dout),
        .wb_we_o(wb_we),
        .wb_stall_i(wb_stall),
        .wb_ack_i(wb_ack),

        // Control
        .wbc_grant_i(wbc_grant),
        .wbc_grant_valid_i(wbc_grant_valid)
    );

    task run;
        // Initialize
        wbc_cycle  = '0;
        wbc_strobe = '0;
        wbc_addr[0] = 20'h12345;
        wbc_addr[1] = 20'h67890;
        wbc_dout[0] = 8'hAA;
        wbc_dout[1] = 8'hBB;
        wbc_we     = '0;
        wb_din     = 8'hCC;
        wb_stall   = 1'b0;
        wb_ack     = 1'b0;
        wbc_grant  = 1'b0;
        wbc_grant_valid = 1'b1;

        #1;

        // Test 1: Select controller 0, verify bus outputs
        $display("[%t]   Test 1: Controller 0 selected", $time);
        wbc_cycle[0]  = 1'b1;
        wbc_strobe[0] = 1'b1;
        wbc_we[0]     = 1'b1;
        wbc_grant     = 1'b0;
        #1;
        `assert_equal(wb_cycle, 1'b1);
        `assert_equal(wb_strobe, 1'b1);
        `assert_equal(wb_addr, 20'h12345);
        `assert_equal(wb_dout, 8'hAA);
        `assert_equal(wb_we, 1'b1);
        $display("[%t]   PASS: Controller 0 signals routed to bus", $time);

        // Test 2: Select controller 1, verify bus outputs
        $display("[%t]   Test 2: Controller 1 selected", $time);
        wbc_cycle[0]  = 1'b0;
        wbc_strobe[0] = 1'b0;
        wbc_we[0]     = 1'b0;
        wbc_cycle[1]  = 1'b1;
        wbc_strobe[1] = 1'b1;
        wbc_we[1]     = 1'b0;
        wbc_grant     = 1'b1;
        #1;
        `assert_equal(wb_cycle, 1'b1);
        `assert_equal(wb_strobe, 1'b1);
        `assert_equal(wb_addr, 20'h67890);
        `assert_equal(wb_dout, 8'hBB);
        `assert_equal(wb_we, 1'b0);
        $display("[%t]   PASS: Controller 1 signals routed to bus", $time);

        // Test 3: Verify din is broadcast to all controllers
        $display("[%t]   Test 3: Bus din broadcast to all controllers", $time);
        wb_din = 8'hDD;
        #1;
        `assert_equal(wbc_din[0], 8'hDD);
        `assert_equal(wbc_din[1], 8'hDD);
        $display("[%t]   PASS: din broadcast to all controllers", $time);

        // Test 4: Verify stall routing - selected gets bus stall, others get stall=1
        $display("[%t]   Test 4: Stall routing", $time);
        wb_stall = 1'b0;
        wbc_grant = 1'b1;
        #1;
        `assert_equal(wbc_stall[0], 1'b1);  // Not selected, always stalled
        `assert_equal(wbc_stall[1], 1'b0);  // Selected, gets bus stall
        wb_stall = 1'b1;
        #1;
        `assert_equal(wbc_stall[1], 1'b1);  // Selected, gets bus stall
        $display("[%t]   PASS: Stall routing correct", $time);

        // Test 5: Verify ack routing - only selected gets ack
        $display("[%t]   Test 5: Ack routing", $time);
        wb_ack = 1'b1;
        #1;
        `assert_equal(wbc_ack[0], 1'b0);  // Not selected, no ack
        `assert_equal(wbc_ack[1], 1'b1);  // Selected, gets ack
        $display("[%t]   PASS: Ack routing correct", $time);

        // Test 6: Grant invalid blocks strobe and stalls selected controller
        $display("[%t]   Test 6: Grant invalid blocks new requests", $time);
        wbc_grant_valid = 1'b0;
        wbc_strobe[1] = 1'b1;
        wb_stall = 1'b0;
        #1;
        `assert_equal(wb_strobe, 1'b0);     // Strobe blocked
        `assert_equal(wbc_stall[1], 1'b1);  // Selected controller stalled
        $display("[%t]   PASS: Grant invalid blocks strobe and stalls controller", $time);

        // Test 7: Grant valid allows strobe through
        $display("[%t]   Test 7: Grant valid allows strobe", $time);
        wbc_grant_valid = 1'b1;
        #1;
        `assert_equal(wb_strobe, 1'b1);     // Strobe passes through
        `assert_equal(wbc_stall[1], 1'b0);  // Selected controller not stalled (wb_stall=0)
        $display("[%t]   PASS: Grant valid allows strobe through", $time);
    endtask

    `TB_INIT
endmodule
