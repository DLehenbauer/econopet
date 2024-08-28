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

module keyboard_tb;
    logic                     clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic [WB_ADDR_WIDTH-1:0] addr;
    logic [   DATA_WIDTH-1:0] poci;
    logic [   DATA_WIDTH-1:0] pico;
    logic                     we;
    logic                     cycle;
    logic                     strobe;
    logic                     stall;
    logic                     ack;

    keyboard keyboard (
        .wb_clock_i(clock),
        .wb_addr_i(common_pkg::wb_kbd_addr(addr)),
        .wb_data_i(pico),
        .wb_data_o(poci),
        .wb_we_i(we),
        .wb_cycle_i(cycle),
        .wb_strobe_i(strobe),
        .wb_stall_o(stall),
        .wb_ack_o(ack),

        .pia1_rs_i(),
        .pia1_cs_i()
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
        integer row;

        $display("[%t] BEGIN %m", $time);

        wb.reset;

        $display("[%t]   Keyboard rows must be initialized to 8'hFF at power on.", $time);
        for (row = 0; row < KBD_ROW_COUNT; row = row + 1) begin
            wb.read(row, data_rd, ack_rd);
            `assert_equal(data_rd, 8'hFF);
        end

        $display("[%t]   Wishbone must be able to read/write all rows.", $time);

        // First pass read/writes unique values to all rows.
        for (row = 0; row < KBD_ROW_COUNT; row = row + 1) begin
            wb.write(row, { 4'h5, row[3:0] });
            wb.read(row, data_rd, ack_rd);
            `assert_equal(data_rd, { 4'h5, row[3:0] });
        end

        // Second pass ensures unique values were not overwritten and resets all rows to 8'hFF.
        for (row = 0; row < KBD_ROW_COUNT; row = row + 1) begin
            wb.read(row, data_rd, ack_rd);
            `assert_equal(data_rd, { 4'h5, row[3:0] });
            wb.write(row, 8'hff);
        end

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
