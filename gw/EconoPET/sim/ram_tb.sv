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

module ram_tb;
    logic                     clock;
    logic [WB_ADDR_WIDTH-1:0] addr;
    logic [   DATA_WIDTH-1:0] poci;
    logic [   DATA_WIDTH-1:0] pico;
    logic                     we;
    logic                     cycle;
    logic                     strobe;
    logic                     stall;
    logic                     ack;

    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));

    initial clock_gen.start;
    logic                      ram_oe_o;
    logic                      ram_we_o;
    logic [RAM_ADDR_WIDTH-1:0] ram_addr_o;
    logic [    DATA_WIDTH-1:0] ram_data_i;
    logic [    DATA_WIDTH-1:0] ram_data_o;
    logic                      ram_data_oe;

    ram ram (
        .wb_clock_i(clock),
        .wb_addr_i(common_pkg::wb_ram_addr(addr)),
        .wb_data_i(pico),
        .wb_data_o(poci),
        .wb_we_i(we),
        .wb_cycle_i(cycle),
        .wb_strobe_i(strobe),
        .wb_stall_o(stall),
        .wb_ack_o(ack),
        .ram_oe_o(ram_oe_o),
        .ram_we_o(ram_we_o),
        .ram_addr_o(ram_addr_o),
        .ram_data_i(ram_data_i),
        .ram_data_o(ram_data_o),
        .ram_data_oe(ram_data_oe)
    );

    mock_ram mock_ram (
        .clock_i(clock),
        .ram_oe_n_i(!ram_oe_o),
        .ram_we_n_i(!ram_we_o),
        .ram_addr_i(ram_addr_o),
        .ram_data_i(ram_data_o),
        .ram_data_o(ram_data_i)
    );

    wb_driver wb (
        .wb_clock_i(clock),
        .wb_addr_o(addr),
        .wb_data_i(poci),
        .wb_data_o(pico),
        .wb_we_o(we),
        .wb_cycle_o(cycle),
        .wb_strobe_o(strobe),
        .wb_ack_i(ack),
        .wb_stall_i(stall)
    );

    logic [DATA_WIDTH-1:0] data_rd;
    logic                  ack_rd;

    task run;
        $display("[%t] BEGIN %m", $time);

        wb.reset;
        wb.write(common_pkg::wb_ram_addr(17'h00000), 8'h55);
        wb.read(common_pkg::wb_ram_addr(17'h00000), data_rd, ack_rd);
        `assert_equal(data_rd, 8'h55);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
