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

module wb_mux_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    // localparam CC = 2;
    // localparam PC = 2;

    // logic [WB_ADDR_WIDTH-1:0] wbc_addr [CC-1:0];
    // logic [   DATA_WIDTH-1:0] wbc_din [CC-1:0];
    // logic [   DATA_WIDTH-1:0] wbc_dout [CC-1:0];
    // logic [CC-1:0] wbc_we;
    // logic [CC-1:0] wbc_cycle;
    // logic [CC-1:0] wbc_strobe;
    // logic [CC-1:0] wbc_stall;
    // logic [CC-1:0] wbc_ack;

    // logic [WB_ADDR_WIDTH-1:0] wbp_addr [PC-1:0];
    // logic [   DATA_WIDTH-1:0] wbp_din [PC-1:0];
    // logic [   DATA_WIDTH-1:0] wbp_dout [PC-1:0];
    // logic [PC-1:0] wbp_we;
    // logic [PC-1:0] wbp_cycle;
    // logic [PC-1:0] wbp_strobe;
    // logic [PC-1:0] wbp_stall;
    // logic [PC-1:0] wbp_ack;

    // assign wbc_addr[0] = 'hadd4_0;
    // assign wbc_addr[1] = 'hadd4_1;

    // wire [WB_ADDR_WIDTH-1:0] wbp0_addr;
    // assign wbp0_addr = wbp_addr[0];

    // wb_mux #(
    //     .CC(CC),
    //     .PC(PC)
    // ) wb_mux (
    //     .wbc_addr_i({ wbc_addr[0], wbc_addr[1] }),
    //     .wbp_addr_o({ wbp_addr[0], wbp_addr[1] }),
    //     .wbc_sel_i(1'b0),
    //     .wbp_sel_i(1'b0)
    // );

    task run;
        $display("[%t] BEGIN %m", $time);

        @(posedge clock);
        @(posedge clock);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
