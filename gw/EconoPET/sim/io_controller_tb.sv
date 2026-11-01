// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

module io_controller_tb;
    localparam int IO_ADDR_WIDTH = 7;
    localparam int IO_OFFSET_COUNT = 1 << IO_ADDR_WIDTH;
    localparam int PIA1_OFFSET = 'h10;
    localparam int PIA2_OFFSET = 'h20;
    localparam int VIA_OFFSET  = 'h40;
    localparam logic [2:0] CS_NONE = 3'b000;
    localparam logic [2:0] CS_PIA1 = 3'b100;
    localparam logic [2:0] CS_PIA2 = 3'b010;
    localparam logic [2:0] CS_VIA  = 3'b001;

    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic cpu_slot_be;
    logic cpu_clock;
    logic cpu_data_strobe;
    logic cpu_hold_strobe;
    logic [DATA_WIDTH-1:0] cpu_data;

    timing timing (
        .sys_clock_i(clock),
        .clk16_en_o(),
        .clk8_en_o(),
        .cpu_be_o(cpu_slot_be),
        .cpu_clock_o(cpu_clock),
        .soft_cpu_clock_o(),
        .cpu_addr_strobe_o(),
        .cpu_data_strobe_o(cpu_data_strobe),
        .cpu_data_en_o(),
        .cpu_hold_strobe_o(cpu_hold_strobe),
        .cpu_wr_en_o(),
        .load_sr1_o(),
        .load_sr2_o(),
        .grant_o(),
        .grant_valid_o()
    );

    logic [WB_ADDR_WIDTH-1:0] wb_addr;
    logic [DATA_WIDTH-1:0] wb_dout;
    logic [DATA_WIDTH-1:0] wb_din;
    logic wb_we;
    logic wb_cycle;
    logic wb_strobe;
    logic wb_stall;
    logic wb_ack;

    logic bus_reserve;
    logic bus_own;
    logic [CPU_ADDR_WIDTH-1:0] bus_addr;
    logic [DATA_WIDTH-1:0] bus_data;
    logic bus_data_oe;
    logic bus_we;
    logic io_oe;
    logic pia1_cs;
    logic pia2_cs;
    logic via_cs;
    logic [DATA_WIDTH-1:0] read_data = 8'hff;
    logic cpu_halted = 1'b1;

    assign cpu_data = (bus_own && !bus_we) ? read_data : 'x;

    io_controller dut (
        .wb_clock_i(clock),
        .wbp_addr_i(wb_addr),
        .wbp_data_i(wb_dout),
        .wbp_data_o(wb_din),
        .wbp_we_i(wb_we),
        .wbp_cycle_i(wb_cycle),
        .wbp_strobe_i(wb_strobe),
        .wbp_stall_o(wb_stall),
        .wbp_ack_o(wb_ack),
        .wbp_sel_i(1'b1),

        .cpu_ready_i(1'b0),
        .cpu_halted_i(cpu_halted),
        .cpu_slot_be_i(cpu_slot_be),
        .cpu_data_strobe_i(cpu_data_strobe),
        .cpu_hold_strobe_i(cpu_hold_strobe),
        .cpu_data_i(cpu_data),

        .bus_reserve_o(bus_reserve),
        .bus_own_o(bus_own),
        .bus_addr_o(bus_addr),
        .bus_data_o(bus_data),
        .bus_data_oe_o(bus_data_oe),
        .bus_we_o(bus_we),
        .io_oe_o(io_oe),
        .pia1_cs_o(pia1_cs),
        .pia2_cs_o(pia2_cs),
        .via_cs_o(via_cs)
    );

    // Presents a single Wishbone request and drops its strobe after capture.
    task start_request(
        input logic [6:0] addr_i,
        input logic we_i,
        input logic [DATA_WIDTH-1:0] data_i
    );
        wb_addr = common_pkg::wb_io_addr(addr_i);
        wb_dout = data_i;
        wb_we   = we_i;
        @(negedge clock);
        wb_cycle  = 1'b1;
        wb_strobe = 1'b1;
        @(posedge clock);
        @(negedge clock);
        wb_strobe = 1'b0;
    endtask

    // Checks a complete physical I/O access, including its latched chip select.
    task check_request(
        input logic [6:0] addr_i,
        input logic we_i,
        input logic [DATA_WIDTH-1:0] data_i,
        input logic [2:0] expected_cs
    );
        start_request(addr_i, we_i, data_i);
        wb_addr = common_pkg::wb_io_addr(IO_ADDR_WIDTH'(0));
        @(posedge bus_own);
        #1;
        `assert_equal(bus_reserve, 1'b1)
        `assert_equal(bus_addr, {8'he8, 1'b0, addr_i})
        `assert_equal(bus_we, we_i)
        `assert_equal(io_oe, 1'b1)
        `assert_equal({pia1_cs, pia2_cs, via_cs}, expected_cs)
        `assert_equal(bus_data_oe, we_i)
        if (we_i) `assert_equal(bus_data, data_i)

        @(posedge cpu_clock);
        `assert_equal(bus_own, 1'b1)
        @(negedge cpu_clock);
        `assert_equal(bus_own, 1'b1)

        @(posedge wb_ack);
        #1;
        `assert_equal(bus_own, 1'b0)
        `assert_equal(bus_reserve, 1'b0)
        `assert_equal(io_oe, 1'b0)
        `assert_equal({pia1_cs, pia2_cs, via_cs}, CS_NONE)
        `assert_equal(bus_data_oe, 1'b0)
        wb_cycle = 1'b0;
        @(posedge clock);
    endtask

    // Holes acknowledge immediately and stay electrically idle through delayed ACK consumption.
    task check_unmapped_request(input logic [IO_ADDR_WIDTH-1:0] addr_i, input logic we_i);
        start_request(addr_i, we_i, 8'ha5);
        repeat (4) begin
            `assert_equal(wb_ack, 1'b1)
            `assert_equal(wb_stall, 1'b0)
            `assert_equal(wb_din, 8'hff)
            `assert_equal(bus_reserve, 1'b0)
            `assert_equal(bus_own, 1'b0)
            `assert_equal(bus_data_oe, 1'b0)
            `assert_equal(io_oe, 1'b0)
            `assert_equal({pia1_cs, pia2_cs, via_cs}, CS_NONE)
            @(posedge clock);
            #1;
        end
        wb_cycle = 1'b0;
        @(posedge clock);
    endtask

    // Exercises every mirrored register offset and both unmapped read/write paths.
    task run;
        wb_addr = '0;
        wb_dout = '0;
        wb_we = 1'b0;
        wb_cycle = 1'b0;
        wb_strobe = 1'b0;

        // A pending halt must keep both reservation and external drivers inactive.
        cpu_halted = 1'b0;
        start_request(7'h10, 1'b0, 8'h00);
        repeat (128) begin
            @(negedge clock);
            `assert_equal(bus_reserve, 1'b0)
            `assert_equal(bus_own, 1'b0)
            `assert_equal(wb_ack, 1'b0)
            `assert_equal(wb_stall, 1'b1)
        end
        cpu_halted = 1'b1;
        @(posedge wb_ack);
        wb_cycle = 1'b0;
        @(posedge clock);
        @(negedge clock);

        check_request(7'h10, 1'b1, 8'ha5, CS_PIA1);
        check_request(7'h2f, 1'b1, 8'h5a, CS_PIA2);
        check_request(7'h70, 1'b1, 8'h3c, CS_VIA);

        read_data = 8'h96;
        check_request(7'h12, 1'b0, 8'hxx, CS_PIA1);
        `assert_equal(wb_din, 8'h96)

        for (int offset = 0; offset < IO_OFFSET_COUNT; offset++) begin
            for (int write_mode = 0; write_mode < 2; write_mode++) begin
                if (offset < PIA1_OFFSET) begin
                    check_unmapped_request(IO_ADDR_WIDTH'(offset), write_mode != 0);
                end else begin
                    check_request(
                        IO_ADDR_WIDTH'(offset), write_mode != 0, 8'ha5,
                        offset < PIA2_OFFSET ? CS_PIA1 : offset < VIA_OFFSET ? CS_PIA2 : CS_VIA);
                    if (write_mode == 0) `assert_equal(wb_din, read_data)
                end
            end
        end

        `assert_equal(common_pkg::wb_crtc_addr(CRTC_ADDR_REG_WIDTH'(0)), WB_CRTC_BASE_ADDR)
    endtask

    `TB_INIT
endmodule
