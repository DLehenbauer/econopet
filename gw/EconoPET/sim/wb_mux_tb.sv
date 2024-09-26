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
`include "./src/common_pkg.svh"

import common_pkg::*;

module wb_mux_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    localparam CC = 1;

    logic [WB_ADDR_WIDTH-1:0][CC-1:0] wbc_addr;
    logic [   DATA_WIDTH-1:0][CC-1:0] wbc_din;
    logic [   DATA_WIDTH-1:0][CC-1:0] wbc_dout;
    logic [CC-1:0] wbc_we;
    logic [CC-1:0] wbc_cycle;
    logic [CC-1:0] wbc_strobe;
    logic [CC-1:0] wbc_stall;
    logic [CC-1:0] wbc_ack;

    wb_mux #(
        /* CC: */ 1,
        /* PC: */ 1
    ) wb_mux (
        .wbc_addr_o(wbc_addr),
        .wbc_data_o(wbc_din),
        .wbc_data_i(wbc_dout),
        .wbc_we_o(wbc_we),
        .wbc_cycle_o(wbc_cycle),
        .wbc_strobe_o(wbc_strobe),
        .wbc_stall_i(wbc_stall),
        .wbc_ack_i(wbc_ack)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
