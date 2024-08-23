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

module bram_tb #(
    parameter DATA_DEPTH = 1024,
    parameter RAM_ADDR_WIDTH = $clog2(DATA_DEPTH - 1)
);
    logic                     clock;
    logic [WB_ADDR_WIDTH-1:0] addr;
    logic [   DATA_WIDTH-1:0] poci;
    logic [   DATA_WIDTH-1:0] pico;
    logic                     we;
    logic                     cycle;
    logic                     strobe;
    logic                     ack;

    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));

    initial clock_gen.start;

    bram mem (
        .wb_clock_i(clock),
        .wb_reset_i('0),
        .wb_addr_i(addr[RAM_ADDR_WIDTH-1:0]),
        .wb_data_i(pico),
        .wb_data_o(poci),
        .wb_we_i(we),
        .wb_cycle_i(cycle),
        .wb_strobe_i(strobe),
        .wb_ack_o(ack)
    );

    wb_driver wb (
        .wb_clock_i(clock),
        .wb_addr_o(addr),
        .wb_data_i(poci),
        .wb_data_o(pico),
        .wb_we_o(we),
        .wb_cycle_o(cycle),
        .wb_strobe_o(strobe),
        .wb_ack_i(ack)
    );

    logic [DATA_WIDTH-1:0] data_rd;
    logic                  ack_rd;

    task run;
        $display("[%t] BEGIN %m", $time);

        wb.reset;
        wb.write(10'h00, 8'h55);
        wb.read(10'h00, data_rd, ack_rd);
        `assert_equal(data_rd, 8'h55);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
