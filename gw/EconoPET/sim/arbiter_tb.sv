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

module arbiter_tb #(
    parameter CLK_MHZ = 64
);
    logic wb_clock;
    clock_gen #(CLK_MHZ) clock_gen (.clock_o(wb_clock));
    initial clock_gen.start;

    logic wb_reset = '0;
    logic wbc_cycle = '0;
    logic wbc_strobe = '0;
    logic wbc_stall;
    logic wbc_ack;
    logic wbp_cycle;
    logic wbp_strobe;
    logic wbp_stall = '0;
    logic wbp_ack = '0;
    logic grant = '0;

    arbiter abriter (
        .wb_clock_i(wb_clock),
        .wb_reset_i(wb_reset),
        .wbc_cycle_i(wbc_cycle),
        .wbc_strobe_i(wbc_strobe),
        .wbc_stall_o(wbc_stall),
        .wbc_ack_o(wbc_ack),
        .wbp_cycle_o(wbp_cycle),
        .wbp_strobe_o(wbp_strobe),
        .wbp_stall_i(wbp_stall),
        .wbp_ack_i(wbp_ack),
        .grant_i(grant)
    );

    task static check(logic stall, logic ack, logic cycle, logic strobe);
        $display("[%t]            Arbiter: grant=%d", $time, grant);
        $display("[%t]         Controller: stall=%d, ack=%d", $time, wbc_stall, wbc_ack);
        `assert_equal(wbc_stall, stall);
        `assert_equal(wbc_ack, ack);
        $display("[%t]         Peripheral: cycle=%d, strobe=%d", $time, wbp_cycle, wbp_strobe);
        `assert_equal(wbp_cycle, cycle);
        `assert_equal(wbp_strobe, strobe);
    endtask

    task static run;
        $display("[%t] BEGIN %m", $time);

        $display("[%t]     No grant / No Request", $time);
        check(/* stall: */ 1,  /* ack: */ 0,  /* cycle: */ 0,  /* strobe: */ 0);
        @(posedge wb_clock);

        $display("[%t]     No grant / Controller request stalled", $time);
        wbc_cycle  = 1;
        wbc_strobe = 1;
        @(posedge wb_clock);
        check(/* stall: */ '1,  /* ack: */ '0,  /* cycle: */ '0,  /* strobe: */ '0);

        $display("[%t]     Grant / Controller request pass through", $time);
        grant = 1;
        @(posedge wb_clock);
        check(/* stall: */ '0,  /* ack: */ '0,  /* cycle: */ '1,  /* strobe: */ '1);

        $display("[%t]     Grant / Peripheral stall pass through", $time);
        wbc_strobe = 0; wbp_stall = '1;
        @(posedge wb_clock);
        check(/* stall: */ '1,  /* ack: */ '0,  /* cycle: */ '1,  /* strobe: */ '0);

        $display("[%t]     Grant / Peripheral ack pass through", $time);
        wbp_stall = '0; wbp_ack = '1;
        @(posedge wb_clock);
        check(/* stall: */ '0,  /* ack: */ '1,  /* cycle: */ '1,  /* strobe: */ '0);

        $display("[%t]     No grant / No request", $time);
        grant = '0; wbp_ack = '0;
        @(posedge wb_clock);
        check(/* stall: */ '1,  /* ack: */ '0,  /* cycle: */ '0,  /* strobe: */ '0);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
