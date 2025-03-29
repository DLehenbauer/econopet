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

    logic [ VIA_RS_WIDTH-1:0] rs;

    logic                      cpu_be        = '0;
    logic [CPU_ADDR_WIDTH-1:0] cpu_addr;
    logic [    DATA_WIDTH-1:0] cpu_din;
    logic [    DATA_WIDTH-1:0] cpu_dout;
    logic                      cpu_doe;
    logic                      cpu_we        = '0;
    logic                      cpu_wr_strobe = '0;

    wire pia1_cs = cpu_addr[CPU_ADDR_WIDTH-1:4] == 12'he81;
    wire pia2_cs = cpu_addr[CPU_ADDR_WIDTH-1:4] == 12'he82;
    wire via_cs  = cpu_addr[CPU_ADDR_WIDTH-1:4] == 12'he84;

    logic [KBD_COL_COUNT-1:0][KBD_ROW_COUNT-1:0] usb_kbd;

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
        .usb_kbd_o(usb_kbd)
    );

    io io (
        .wb_clock_i(clock),
        .cpu_be_i(cpu_be),
        .cpu_wr_strobe_i(cpu_wr_strobe),
        .cpu_data_i(cpu_din),
        .cpu_data_o(cpu_dout),
        .cpu_data_oe(cpu_doe),
        .cpu_we_i(cpu_we),
        .pia1_cs_i(pia1_cs),
        .pia2_cs_i(pia2_cs),
        .via_cs_i(via_cs),
        .rs_i(cpu_addr[VIA_RS_WIDTH-1:0]),
        .usb_kbd_i(usb_kbd)
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

    task cpu_write(
        input logic [CPU_ADDR_WIDTH-1:0] addr,
        input logic [    DATA_WIDTH-1:0] data
    );
        @(negedge clock);

        cpu_be        = 1'b1;
        cpu_addr      = addr;
        cpu_din       = data;
        cpu_we        = 1'b1;
        cpu_wr_strobe = 1'b1;
        
        @(negedge clock);
        
        cpu_we        = 1'b0;
        cpu_wr_strobe = 1'b0;
        cpu_be        = 1'b0;

        repeat (16) @(posedge clock);
    endtask

    task cpu_read(
        input  logic [CPU_ADDR_WIDTH-1:0] addr,
        output logic [    DATA_WIDTH-1:0] data
    );
        @(negedge clock);

        cpu_be        = 1'b1;
        cpu_addr      = addr;
        cpu_we        = 1'b0;
        cpu_wr_strobe = 1'b0;

        @(negedge clock);

        data          = cpu_dout;
        cpu_be        = 1'b0;

        repeat (16) @(posedge clock);
    endtask

    task cpu_write_pia1(
        input logic [PIA_RS_WIDTH-1:0] rs,
        input logic [  DATA_WIDTH-1:0] data
    );
        cpu_write(16'hE810 + rs, data);
    endtask

    task cpu_read_pia1(
        input  logic [PIA_RS_WIDTH-1:0] rs,
        output logic [  DATA_WIDTH-1:0] data
    );
        cpu_read(16'hE810 + rs, data);
    endtask

    task cpu_select_col(
        input logic [DATA_WIDTH-1:0] col
    );
        cpu_write_pia1(PIA_PORTA, col);
    endtask

    task cpu_read_current_col (
        output logic [DATA_WIDTH-1:0] data
    );
        cpu_read_pia1(PIA_PORTB, data);
    endtask

    task run;
        integer col;
        logic [DATA_WIDTH-1:0] value;
        logic [DATA_WIDTH-1:0] data;

        $display("[%t] BEGIN %m", $time);

        wb.reset;

        $display("[%t]   Keyboard cols must be initialized to 8'hFF at power on.", $time);
        for (col = 0; col < KBD_COL_COUNT; col = col + 1) begin
            wb.read(col, data_rd);
            $display("[%t]     col %0d = %2h %x", $time, col, data_rd, usb_kbd[col]);
            `assert_equal(data_rd, 8'hFF);
        end

        $display("[%t]   Wishbone must be able to read/write all cols.", $time);

        // First pass read/writes unique values to all cols.
        for (col = 0; col < KBD_COL_COUNT; col = col + 1) begin
            value = { 4'h5, col[3:0] };
            wb.write(col, value);
            $display("[%t]     col %0d <- %2h (WB)", $time, col, value);

            cpu_select_col(col);
            cpu_read_current_col(data);
            $display("[%t]     col %0d -> %2h (CPU)", $time, col, data);
            `assert_equal(data, value);
            
            wb.read(col, data_rd);
            $display("[%t]     col %0d -> %2h (WB)", $time, col, data_rd);
            `assert_equal(data_rd, value);
        end

        // Second pass ensures unique values were not overwritten and resets all cols to 8'hFF.
        for (col = 0; col < KBD_COL_COUNT; col = col + 1) begin
            wb.read(col, data_rd);
            $display("[%t]     col %0d -> %2h (WB)", $time, col, data_rd);
            `assert_equal(data_rd, { 4'h5, col[3:0] });
            wb.write(col, 8'hff);
        end

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
