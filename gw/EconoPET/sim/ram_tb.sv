// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

module ram_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic [ WB_ADDR_WIDTH-1:0] addr;
    logic [    DATA_WIDTH-1:0] poci;
    logic [    DATA_WIDTH-1:0] pico;
    logic                      we;
    logic                      cycle;
    logic                      strobe;
    logic                      stall;
    logic                      ack;

    logic                      ram_oe_o;
    logic                      ram_we_o;
    logic [RAM_ADDR_WIDTH-1:0] ram_addr_o;
    logic [    DATA_WIDTH-1:0] ram_data_i;
    logic [    DATA_WIDTH-1:0] ram_data_o;
    logic                      ram_data_oe;

    logic [    DATA_WIDTH-1:0] sram_data_o;
    logic                      sram_data_oe;
    logic [    DATA_WIDTH-1:0] ram_data_bus;
    wire [1:0] data_drivers = {sram_data_oe, ram_data_oe};

    always_comb begin
        case (data_drivers)
            2'b01:   ram_data_bus = ram_data_o;
            2'b10:   ram_data_bus = sram_data_o;
            default: ram_data_bus = {DATA_WIDTH{1'bx}};  // Undriven or contended
        endcase
    end
    assign ram_data_i   = ram_data_bus;

    always_ff @(posedge clock) begin
        assert($onehot0(data_drivers))
            else $fatal(1, "[%0t] Multiple RAM data bus drivers: {sram, fpga}=%b", $time, data_drivers);
    end

    ram ram (
        .wb_clock_i(clock),
        .wbp_addr_i(common_pkg::wb_ram_addr(addr)),
        .wbp_data_i(pico),
        .wbp_data_o(poci),
        .wbp_we_i(we),
        .wbp_cycle_i(cycle),
        .wbp_strobe_i(strobe),
        .wbp_stall_o(stall),
        .wbp_ack_o(ack),
        .wbp_sel_i(1'b1),
        .ram_oe_o(ram_oe_o),
        .ram_we_o(ram_we_o),
        .ram_addr_o(ram_addr_o),
        .ram_data_i(ram_data_i),
        .ram_data_o(ram_data_o),
        .ram_data_oe(ram_data_oe)
    );

    mock_sram mock_sram (
        .addr_i(ram_addr_o),
        .data_i(ram_data_bus),
        .data_o(sram_data_o),
        .data_oe_o(sram_data_oe),
        .ce_ni(1'b0),
        .oe_ni(!ram_oe_o),
        .we_ni(!ram_we_o)
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

    task test_rd(input logic [WB_ADDR_WIDTH-1:0] addr, input logic [DATA_WIDTH-1:0] data);
        logic [DATA_WIDTH-1:0] data_rd;

        wb.read(common_pkg::wb_ram_addr(addr), data_rd);
        `assert_equal(data_rd, data);
    endtask

    task test_wr(input logic [WB_ADDR_WIDTH-1:0] addr, input logic [DATA_WIDTH-1:0] data);
        logic [DATA_WIDTH-1:0] data_rd;

        wb.write(common_pkg::wb_ram_addr(addr), data);
    endtask

    task test_rw(input logic [WB_ADDR_WIDTH-1:0] addr, input logic [DATA_WIDTH-1:0] data);
        test_wr(addr, data);
        test_rd(addr, data);
    endtask

    task run;
        wb.reset;
        test_wr(17'h00000, 8'h55);
        test_wr(17'h1fffe, 8'h33);
        test_wr(17'h1ffff, 8'hcc);

        test_rd(17'h00000, 8'h55);
        test_rd(17'h1fffe, 8'h33);
        test_rd(17'h1ffff, 8'hcc);

        // If PARANOID is defined, exhaustively test all RAM addresses:
`ifdef PARANOID
        for (int i = 0; i < 2**RAM_ADDR_WIDTH; i++) begin
            test_rw(i, i[7:0]);
        end
        for (int i = 0; i < 2**RAM_ADDR_WIDTH; i++) begin
            test_rd(i, i[7:0]);
        end
`endif

    endtask

    `TB_INIT
endmodule
