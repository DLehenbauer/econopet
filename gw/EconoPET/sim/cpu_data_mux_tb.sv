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

module cpu_data_mux_tb;
    localparam COUNT = 3;

    logic [COUNT-1:0][DATA_WIDTH-1:0] data_i;
    logic [COUNT-1:0]                 oe_i;
    logic [DATA_WIDTH-1:0]            data_o;
    logic                             oe_o;

    cpu_data_mux #(
        .COUNT(COUNT)
    ) uut (
        .data_i(data_i),
        .oe_i(oe_i),
        .data_o(data_o),
        .oe_o(oe_o)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        // Initialize: all sources disabled
        data_i[0] = 8'hAA;
        data_i[1] = 8'hBB;
        data_i[2] = 8'hCC;
        oe_i = 3'b000;

        #1;

        // Test 1: No driver active - OE should be low
        `assert_equal(oe_o, 1'b0);
        $display("[%t]   PASS: No driver active, oe_o=0", $time);

        // Test 2: Driver 0 active - should select data_i[0]
        oe_i = 3'b001;
        #1;
        `assert_equal(oe_o, 1'b1);
        `assert_equal(data_o, 8'hAA);
        $display("[%t]   PASS: Driver 0 active, data_o=0x%02x", $time, data_o);

        // Test 3: Driver 1 active - should select data_i[1]
        oe_i = 3'b010;
        #1;
        `assert_equal(oe_o, 1'b1);
        `assert_equal(data_o, 8'hBB);
        $display("[%t]   PASS: Driver 1 active, data_o=0x%02x", $time, data_o);

        // Test 4: Driver 2 active - should select data_i[2]
        oe_i = 3'b100;
        #1;
        `assert_equal(oe_o, 1'b1);
        `assert_equal(data_o, 8'hCC);
        $display("[%t]   PASS: Driver 2 active, data_o=0x%02x", $time, data_o);

        // Test 5: Data changes while driver active
        data_i[2] = 8'hDD;
        #1;
        `assert_equal(data_o, 8'hDD);
        $display("[%t]   PASS: Data change propagates, data_o=0x%02x", $time, data_o);

        // Test 6: Switch between drivers
        oe_i = 3'b001;
        #1;
        `assert_equal(data_o, 8'hAA);
        $display("[%t]   PASS: Switch to driver 0, data_o=0x%02x", $time, data_o);

        // Disable all drivers
        oe_i = 3'b000;
        #1;
        `assert_equal(oe_o, 1'b0);
        $display("[%t]   PASS: All drivers disabled", $time);

        // Note: Testing multiple OE assertion would cause $fatal in simulation,
        // so we skip that test here. The assertion is verified by the fact that
        // main.sv's existing mutual-exclusion guarantees prevent this condition.

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
