// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

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

    logic                      cpu_be           = '0;
    logic [CPU_ADDR_WIDTH-1:0] cpu_addr;
    logic [    DATA_WIDTH-1:0] bus_dout;    // Data written by bus (written by CPU, etc.)
    logic [    DATA_WIDTH-1:0] bus_din;     // Data read by bus (read by CPU, etc.)
    logic                      cpu_doe;
    logic                      cpu_we           = '0;
    logic                      cpu_data_strobe  = '0;
    logic                      bridge_access = 1'b0;
    logic [PIA_RS_WIDTH-1:0]   bridge_rs;
    logic                      bridge_we;
    logic [DATA_WIDTH-1:0]     bridge_data;

    wire pia1_cs = cpu_addr[CPU_ADDR_WIDTH-1:4] == 12'he81;
    wire pia2_cs = cpu_addr[CPU_ADDR_WIDTH-1:4] == 12'he82;
    wire via_cs  = cpu_addr[CPU_ADDR_WIDTH-1:4] == 12'he84;

    logic [KBD_COL_COUNT-1:0][KBD_ROW_COUNT-1:0] usb_kbd;

    keyboard keyboard (
        .wb_clock_i(clock),
        .wbp_addr_i(addr),
        .wbp_data_i(pico),
        .wbp_data_o(poci),
        .wbp_we_i(we),
        .wbp_cycle_i(cycle),
        .wbp_strobe_i(strobe),
        .wbp_stall_o(stall),
        .wbp_ack_o(ack),
        .wbp_sel_i(1'b1),

        .cpu_be_i(cpu_be),
        .cpu_data_o(bus_din),       // Data read by bus (read by CPU, etc.)
        .cpu_data_oe(cpu_doe),
        .cpu_we_i(cpu_we),
        .pia1_cs_i(pia1_cs),
        .pia1_rs_i(cpu_addr[PIA_RS_WIDTH-1:0]),
        .pia1_access_i(bridge_access || (pia1_cs && cpu_data_strobe)),
        .pia1_access_rs_i(bridge_access ? bridge_rs : cpu_addr[PIA_RS_WIDTH-1:0]),
        .pia1_access_we_i(bridge_access ? bridge_we : cpu_we),
        .pia1_access_data_i(bridge_access ? bridge_data : bus_dout)
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

        cpu_be          = 1'b1;
        cpu_addr        = addr;
        bus_dout        = data;     // Data written by bus (written by CPU, etc.)
        cpu_we          = 1'b1;
        cpu_data_strobe = 1'b1;
        
        @(negedge clock);
        
        cpu_we          = 1'b0;
        cpu_data_strobe = 1'b0;
        cpu_be          = 1'b0;

        repeat (16) @(posedge clock);
    endtask

    task cpu_read(
        input  logic [CPU_ADDR_WIDTH-1:0] addr,
        output logic [    DATA_WIDTH-1:0] data
    );
        @(negedge clock);

        cpu_be          = 1'b1;
        cpu_addr        = addr;
        cpu_we          = 1'b0;

        // We need a positive edge on the system clock for keyboard to detect that the next
        // CPU read should be intercepted.
        @(negedge clock);

        // The pet keyboard matrix should be updated with the intercepted usb key press (if any)
        // on the later data strobe.  If not intercepted, we assume no key press from PIA1 (0xFF).
        cpu_data_strobe = 1'b1;
        bus_dout        = cpu_doe ? bus_din : 8'hff;    // Data written by bus (in this case FPGA or PIA1)

        @(negedge clock);

        data            = bus_dout;
        cpu_data_strobe = 1'b0;
        cpu_be          = 1'b0;

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

    // Present a captured bridge transaction without enabling CPU injection.
    task bridge_pia1(
        input logic [PIA_RS_WIDTH-1:0] register,
        input logic write,
        input logic [DATA_WIDTH-1:0] data
    );
        @(negedge clock);
        bridge_rs = register;
        bridge_we = write;
        bridge_data = data;
        bridge_access = 1'b1;
        @(negedge clock);
        bridge_access = 1'b0;
        repeat (3) @(posedge clock);
        `assert_equal(cpu_doe, 1'b0)
    endtask

    task run;
        integer col;
        logic [DATA_WIDTH-1:0] value;
        logic [DATA_WIDTH-1:0] data;

        wb.reset;
        cpu_addr = '0;

        $display("[%t]   Keyboard cols must be initialized to 8'hFF at power on.", $time);
        for (col = 0; col < KBD_COL_COUNT; col = col + 1) begin
            wb.read(col, data_rd);
            $display("[%t]     col %0d = %2h %x", $time, col, data_rd, usb_kbd[col]);
            `assert_equal(data_rd, 8'hFF);
        end

        $display("[%t]   Wishbone must be able to read/write all cols.", $time);

        // Initialize actual port directions and data-register selection.
        cpu_write_pia1(PIA_PORTA, 8'h0f);
        cpu_write_pia1(PIA_CRA, 8'h04);
        cpu_write_pia1(PIA_CRB, 8'h04);

        // First pass read/writes unique values to all cols.
        for (col = 0; col < KBD_COL_COUNT; col = col + 1) begin
            value = { 4'h5, col[3:0] };
            wb.write(col, value);
            $display("[%t]     col %0d <- %2h (WB)", $time, col, value);

            // Read back returns FF until the CPU reads the column.
            wb.read(col, data_rd);
            $display("[%t]     col %0d -> %2h (WB)", $time, col, data_rd);
            `assert_equal(data_rd, 8'hFF);

            cpu_select_col(col);
            cpu_read_current_col(data);
            $display("[%t]     col %0d -> %2h (CPU)", $time, col, data);
            `assert_equal(data, value);
            
            wb.read(col, data_rd);
            $display("[%t]     col %0d -> %2h (WB)", $time, col, data_rd);
            `assert_equal(data_rd, value);
        end

        // Second pass ensures unique values were not overwritten.
        for (col = 0; col < KBD_COL_COUNT; col = col + 1) begin
            wb.read(col, data_rd);
            $display("[%t]     col %0d -> %2h (WB)", $time, col, data_rd);
            `assert_equal(data_rd, { 4'h5, col[3:0] });
        end

        // Raw bridge scans replace CPU observations without consuming USB injection.
        bridge_pia1(PIA_PORTA, 1'b1, 8'h03);
        bridge_pia1(PIA_PORTB, 1'b0, 8'ha5);
        wb.read(3, data_rd);
        `assert_equal(data_rd, 8'ha5)

        // DDR reads are neither injected nor stored as keyboard samples.
        bridge_pia1(PIA_CRB, 1'b1, 8'h00);
        bridge_pia1(PIA_PORTB, 1'b0, 8'h00);
        cpu_read_current_col(data);
        `assert_equal(data, 8'hff)
        wb.read(3, data_rd);
        `assert_equal(data_rd, 8'ha5)
        bridge_pia1(PIA_CRB, 1'b1, 8'h04);

        // Restoring DDRA must not change the row latch established through the bridge.
        bridge_pia1(PIA_CRA, 1'b1, 8'h00);
        bridge_pia1(PIA_PORTA, 1'b1, 8'h0f);
        bridge_pia1(PIA_CRA, 1'b1, 8'h00);
        bridge_pia1(PIA_PORTA, 1'b1, 8'h0f);
        bridge_pia1(PIA_CRA, 1'b1, 8'h04);
        cpu_read_current_col(data);
        `assert_equal(data, 8'h53)
        wb.read(3, data_rd);
        `assert_equal(data_rd, 8'h53)

        // Input row-select bits float high rather than following the output latch.
        bridge_pia1(PIA_CRA, 1'b1, 8'h00);
        bridge_pia1(PIA_PORTA, 1'b1, 8'h07);
        bridge_pia1(PIA_CRA, 1'b1, 8'h04);
        bridge_pia1(PIA_PORTB, 1'b0, 8'h00);
        cpu_read_current_col(data);
        `assert_equal(data, 8'hff)
        wb.read(3, data_rd);
        `assert_equal(data_rd, 8'h53)
        bridge_pia1(PIA_CRA, 1'b1, 8'h00);
        bridge_pia1(PIA_PORTA, 1'b1, 8'h0f);
        bridge_pia1(PIA_CRA, 1'b1, 8'h04);

        // Invalid row indices do not inject or alter valid cache entries.
        for (col = KBD_COL_COUNT; col < (1 << KBD_COL_WIDTH); ++col) begin
            bridge_pia1(PIA_PORTA, 1'b1, DATA_WIDTH'(col));
            bridge_pia1(PIA_PORTB, 1'b0, 8'h00);
            cpu_read_current_col(data);
            `assert_equal(data, 8'hff)
        end
        wb.read(3, data_rd);
        `assert_equal(data_rd, 8'h53)

    endtask

    `TB_INIT
endmodule
