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

module system_tb #(
    parameter CLK_MHZ = 64,
    parameter DATA_WIDTH = 8,
    parameter WB_ADDR_WIDTH = 20
);
    logic wb_clock;
    clock_gen #(CLK_MHZ) clock_gen (.clock_o(wb_clock));
    initial clock_gen.start;

    logic cpu_clock;
    logic cpu_be;

    logic [WB_ADDR_WIDTH-1:0]   wb_addr;
    logic [DATA_WIDTH-1:0]      wb_poci;
    logic [DATA_WIDTH-1:0]      wb_pico;
    logic                       wb_we;
    logic                       wb_cycle;
    logic                       wb_strobe;
    logic                       wb_ack;
    logic                       wb_stall;

    wb_driver wb_driver (
        .wb_clock_i(wb_clock),
        .wb_addr_o(wb_addr),
        .wb_data_i(wb_poci),
        .wb_data_o(wb_pico),
        .wb_we_o(wb_we),
        .wb_cycle_o(wb_cycle),
        .wb_strobe_o(wb_strobe),
        .wb_stall_i(wb_stall),
        .wb_ack_i(wb_ack)
    );

    system system (
        .wb_clock_i(wb_clock),
        .wb_addr_i(wb_addr),
        .wb_data_i(wb_pico),
        .wb_data_o(wb_poci),
        .wb_we_i(wb_we),
        .wb_cycle_i(wb_cycle),
        .wb_strobe_i(wb_strobe),
        .wb_stall_o(wb_stall),
        .wb_ack_o(wb_ack),
        .cpu_be_o(cpu_be),
        .cpu_clock_o(cpu_clock)
    );

    logic [DATA_WIDTH-1:0] data_rd;
    logic                  ack_rd;

    task run;
        $display("[%t] BEGIN %m", $time);

        wb_driver.reset;
        wb_driver.write(10'h00, 8'h55);
        wb_driver.read(10'h00, data_rd, ack_rd);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
