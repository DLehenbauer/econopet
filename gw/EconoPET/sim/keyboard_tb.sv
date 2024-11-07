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
    logic clock;
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

    logic [ PIA_RS_WIDTH-1:0] pia1_rs;
    logic                     pia1_cs;

    logic [   DATA_WIDTH-1:0] cpu_din;
    logic [   DATA_WIDTH-1:0] cpu_dout;
    logic                     cpu_doe;
    logic                     cpu_we     = '0;
    logic                     cpu_strobe = '0;

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
        .wb_sel_i(1'b1),

        .cpu_data_i(cpu_din),
        .cpu_data_o(cpu_dout),
        .cpu_data_oe(cpu_doe),
        .cpu_we_i(cpu_we),

        .pia1_cs_i(pia1_cs),
        .pia1_rs_i(pia1_rs)
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

    task cpu_select_row(
        input logic [DATA_WIDTH-1:0] row
    );
        @(negedge clock);

        pia1_rs    = PIA_PORTA;
        pia1_cs    = 1'b1;
        cpu_din    = row;
        cpu_we     = 1'b1;
        cpu_strobe = 1'b1;
        
        @(negedge clock);
        
        pia1_cs    = 1'b0;
        cpu_we     = 1'b0;
        cpu_strobe = 1'b0;
    endtask

    task cpu_read_current_row (
        output logic [DATA_WIDTH-1:0] data
    );
        @(negedge clock);

        pia1_rs    = PIA_PORTB;
        pia1_cs    = 1'b1;
        cpu_we     = 1'b0;
        cpu_strobe = 1'b1;

        @(negedge clock);

        data = cpu_dout;        
        pia1_cs    = 1'b0;
        cpu_strobe = 1'b0;
    endtask

    task run;
        integer row;
        logic [DATA_WIDTH-1:0] value;
        logic [DATA_WIDTH-1:0] data;

        $display("[%t] BEGIN %m", $time);

        wb.reset;

        $display("[%t]   Keyboard rows must be initialized to 8'hFF at power on.", $time);
        for (row = 0; row < KBD_ROW_COUNT; row = row + 1) begin
            wb.read(row, data_rd);
            `assert_equal(data_rd, 8'hFF);
        end

        $display("[%t]   Wishbone must be able to read/write all rows.", $time);

        // First pass read/writes unique values to all rows.
        for (row = 0; row < KBD_ROW_COUNT; row = row + 1) begin
            value = { 4'h5, row[3:0] };
            wb.write(row, value);

            cpu_select_row(row);
            cpu_read_current_row(data);
            `assert_equal(data, value);
            
            wb.read(row, data_rd);
            `assert_equal(data_rd, value);
        end

        // Second pass ensures unique values were not overwritten and resets all rows to 8'hFF.
        for (row = 0; row < KBD_ROW_COUNT; row = row + 1) begin
            wb.read(row, data_rd);
            `assert_equal(data_rd, { 4'h5, row[3:0] });
            wb.write(row, 8'hff);
        end

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
