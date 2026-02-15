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

`include "./sim/assert.svh"

import common_pkg::*;

module wbp_mux_tb;
    localparam COUNT = 3;

    // Wishbone bus outputs (from mux)
    logic [DATA_WIDTH-1:0] wb_din;
    logic                  wb_stall;
    logic                  wb_ack;

    // Peripheral inputs (to mux)
    logic [COUNT-1:0][DATA_WIDTH-1:0] wbp_din;
    logic [COUNT-1:0]                 wbp_stall;
    logic [COUNT-1:0]                 wbp_ack;

    // Control
    logic [COUNT-1:0] wbp_sel;

    wbp_mux #(
        .COUNT(COUNT)
    ) dut (
        .wb_din_o(wb_din),
        .wb_stall_o(wb_stall),
        .wb_ack_o(wb_ack),
        .wbp_din_i(wbp_din),
        .wbp_stall_i(wbp_stall),
        .wbp_ack_i(wbp_ack),
        .wbp_sel_i(wbp_sel)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        // Initialize peripheral responses with distinct values
        wbp_din[0]   = 8'hAA;
        wbp_din[1]   = 8'hBB;
        wbp_din[2]   = 8'hCC;
        wbp_stall[0] = 1'b0;
        wbp_stall[1] = 1'b1;
        wbp_stall[2] = 1'b0;
        wbp_ack[0]   = 1'b1;
        wbp_ack[1]   = 1'b0;
        wbp_ack[2]   = 1'b1;
        wbp_sel      = 3'b000;

        #1;

        // Test 1: Select peripheral 0
        wbp_sel = 3'b001;
        #1;
        `assert_equal(wb_din, 8'hAA);
        `assert_equal(wb_stall, 1'b0);
        `assert_equal(wb_ack, 1'b1);
        $display("[%t]   PASS: Peripheral 0 selected", $time);

        // Test 2: Select peripheral 1
        wbp_sel = 3'b010;
        #1;
        `assert_equal(wb_din, 8'hBB);
        `assert_equal(wb_stall, 1'b1);
        `assert_equal(wb_ack, 1'b0);
        $display("[%t]   PASS: Peripheral 1 selected", $time);

        // Test 3: Select peripheral 2
        wbp_sel = 3'b100;
        #1;
        `assert_equal(wb_din, 8'hCC);
        `assert_equal(wb_stall, 1'b0);
        `assert_equal(wb_ack, 1'b1);
        $display("[%t]   PASS: Peripheral 2 selected", $time);

        // Test 4: Data changes propagate
        wbp_din[2] = 8'hDD;
        #1;
        `assert_equal(wb_din, 8'hDD);
        $display("[%t]   PASS: Data change propagates", $time);

        // Test 5: Switch selection
        wbp_sel = 3'b001;
        #1;
        `assert_equal(wb_din, 8'hAA);
        $display("[%t]   PASS: Selection switch works", $time);

        #1 $display("[%t] END %m", $time);
    endtask

    initial begin
        $dumpfile("work_sim/wbp_mux_tb.vcd");
        $dumpvars(0, wbp_mux_tb);
        run;
        $finish;
    end
endmodule
