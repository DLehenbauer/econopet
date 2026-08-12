// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Smoke test for the VIRTUAL (soft) 6502 side of the in-fabric CPU mux.
// Selects the soft m6502 core via REG_CPU_SEL and proves it fetches from RAM,
// executes, and writes back through the FPGA-driven bus -- the same datapath
// the physical 6502 would use, but fully simulatable. Wiring mirrors
// superpet_top_tb (top -> mock_sram + SPI1), the only difference being which
// CPU the register selects and that the 6502 reset vector is at $FFFC/$FFFD.
module superpet_soft6502_tb;
    bit sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) sys_clock_gen (.clock_o(sys_clock));
    initial sys_clock_gen.start;

    logic [CPU_ADDR_WIDTH-1:0] bus_addr;
    wire  [DATA_WIDTH-1:0]     bus_data;
    logic bus_we_n;

    logic [DATA_WIDTH-1:0] bus_data_mux;
    logic                  bus_data_mux_oe;
    assign bus_data = bus_data_mux_oe ? bus_data_mux : {DATA_WIDTH{1'bz}};

    bit   manual_reset_n = 1'b1;
    logic cpu_reset_n;
    logic top_reset_n;
    logic top_reset_n_oe;
    assign cpu_reset_n = top_reset_n_oe ? top_reset_n : manual_reset_n;

    logic cpu_be;
    logic cpu_clock;
    logic cpu_ready;

    logic [CPU_ADDR_WIDTH-1:0] top_addr;
    logic [CPU_ADDR_WIDTH-1:0] top_addr_oe;
    logic [DATA_WIDTH-1:0] top_data;
    logic [DATA_WIDTH-1:0] top_data_oe;
    logic top_we_n;
    logic top_we_n_oe;

    logic ram_addr_a10_o, ram_addr_a11_o, ram_addr_a15_o, ram_addr_a16_o;
    logic ram_oe_n_o, ram_we_n_o;
    logic io_oe_n, pia1_cs_n, pia2_cs_n, via_cs_n;

    logic spi_sck, spi_cs_n, spi_pico, spi_poci, spi_stall;
    logic [7:0] spi_rx_data;

    top top (
        .sys_clock_i(sys_clock),

        .cpu_be_o(cpu_be),
        .cpu_ready_o(cpu_ready),
        .cpu_reset_n_i(cpu_reset_n),
        .cpu_reset_n_o(top_reset_n),
        .cpu_reset_n_oe(top_reset_n_oe),
        .cpu_clock_o(cpu_clock),
        .cpu_addr_i (bus_addr),
        .cpu_addr_o (top_addr),
        .cpu_addr_oe(top_addr_oe),
        .cpu_data_i (bus_data),
        .cpu_data_o (top_data),
        .cpu_data_oe(top_data_oe),
        .cpu_we_n_i (1'b1),
        .cpu_we_n_o (top_we_n),
        .cpu_we_n_oe(top_we_n_oe),

        .cpu_sync_i(1'b0),

        .ram_addr_a10_o(ram_addr_a10_o),
        .ram_addr_a11_o(ram_addr_a11_o),
        .ram_addr_a15_o(ram_addr_a15_o),
        .ram_addr_a16_o(ram_addr_a16_o),
        .ram_oe_n_o(ram_oe_n_o),
        .ram_we_n_o(ram_we_n_o),

        .io_oe_n_o(io_oe_n),
        .pia1_cs_n_o(pia1_cs_n),
        .pia2_cs_n_o(pia2_cs_n),
        .via_cs_n_o(via_cs_n),

        .spi0_cs_ni (spi_cs_n),
        .spi0_sck_i (spi_sck),
        .spi0_sd_i  (spi_pico),
        .spi0_sd_o  (spi_poci),
        .spi_stall_o(spi_stall),

        .graphic_i(1'b0),
        .config_crt_i(1'b0),
        .config_keyboard_i(1'b0)
    );

    wire [RAM_ADDR_WIDTH-1:0] ram_addr = {
        ram_addr_a16_o,
        ram_addr_a15_o,
        bus_addr[14],
        bus_addr[13],
        bus_addr[12],
        ram_addr_a11_o,
        ram_addr_a10_o,
        bus_addr[9:0]
    };

    mock_sram mock_sram (
        .addr_i(ram_addr),
        .data_io(bus_data),
        .ce_ni(1'b0),
        .oe_ni(ram_oe_n_o),
        .we_ni(ram_we_n_o)
    );

    mock_bus mock_bus (
        .clock_i(sys_clock),

        .top_addr_i(top_addr),
        .top_addr_oe_i(top_addr_oe[0]),
        .top_data_i(top_data),
        .top_data_oe_i(top_data_oe[0]),
        .top_we_n_i(top_we_n),
        .top_we_n_oe_i(top_we_n_oe),

        .cpu_be_i(1'b0),
        .cpu_addr_i({CPU_ADDR_WIDTH{1'b0}}),
        .cpu_data_i({DATA_WIDTH{1'b0}}),
        .cpu_we_n_i(1'b1),

        .ram_oe_n_i(ram_oe_n_o),
        .ram_we_n_i(ram_we_n_o),

        .io_data_i(8'h10),
        .io_oe_n_i(io_oe_n),

        .bus_addr_o(bus_addr),
        .bus_data_o(bus_data_mux),
        .bus_data_oe_o(bus_data_mux_oe),
        .bus_we_n_o(bus_we_n)
    );

    spi1_driver spi1_driver (
        .clock_i(sys_clock),
        .spi_sck_o(spi_sck),
        .spi_cs_no(spi_cs_n),
        .spi_pico_o(spi_pico),
        .spi_poci_i(spi_poci),
        .spi_stall_i(spi_stall),
        .spi_data_o(spi_rx_data)
    );

    task static spi_read (output logic [DATA_WIDTH-1:0] data_o);
        spi1_driver.read_next;
        data_o = spi_rx_data;
    endtask

    task static spi_read_at (
        input  logic [WB_ADDR_WIDTH-1:0] addr_i,
        output logic [   DATA_WIDTH-1:0] data_o
    );
        spi1_driver.read_at(addr_i);
        spi_read(data_o);
    endtask

    task static spi_write_at (
        input logic [WB_ADDR_WIDTH-1:0] addr_i,
        input logic [   DATA_WIDTH-1:0] data_i
    );
        spi1_driver.write_at(addr_i, data_i);
    endtask

    task static run;
        logic [DATA_WIDTH-1:0] dout;

        $display("[%t] BEGIN soft-6502 smoke test", $time);
        spi1_driver.reset;

        // Small 6502 test program at $0300:
        //   $0300: A9 42        LDA #$42
        //   $0302: 8D 00 02     STA $0200
        //   $0305: 4C 05 03     JMP $0305   (infinite self-loop)
        $display("[%t]   Loading 6502 test program at $0300", $time);
        spi_write_at(common_pkg::wb_ram_addr(17'h00300), 8'hA9);
        spi_write_at(common_pkg::wb_ram_addr(17'h00301), 8'h42);
        spi_write_at(common_pkg::wb_ram_addr(17'h00302), 8'h8D);
        spi_write_at(common_pkg::wb_ram_addr(17'h00303), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h00304), 8'h02);
        spi_write_at(common_pkg::wb_ram_addr(17'h00305), 8'h4C);
        spi_write_at(common_pkg::wb_ram_addr(17'h00306), 8'h05);
        spi_write_at(common_pkg::wb_ram_addr(17'h00307), 8'h03);

        // Poison the target so a real write is observable.
        spi_write_at(common_pkg::wb_ram_addr(17'h00200), 8'h00);

        // 6502 reset vector is at $FFFC/$FFFD -> $0300.
        $display("[%t]   Writing 6502 reset vector -> $0300", $time);
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFC), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFD), 8'h03);

        // Select the soft 6502.
        $display("[%t]   Selecting soft 6502 (REG_CPU_SEL=%0d)", $time, CPU_SEL_SOFT_6502);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6502));

        // Reset pulse with READY=1 (REG_CPU bit0=READY, bit1=RESET). The soft
        // 6502's i_rdy is the CPU-ready line, so it must be high to run.
        $display("[%t]   Reset pulse + ready via REG_CPU", $time);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0011);  // READY=1, RESET=1
        #20000;
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0001);  // READY=1, RESET=0

        // Soft 6502 runs at ~1MHz (cpu_clock_o); ~3 instructions + reset seq.
        $display("[%t]   Waiting for program to execute", $time);
        #200000;

        $display("[%t]   RAM[$0200] (direct peek) = $%02x (expect $42)", $time, mock_sram.mem[17'h00200]);
        `assert_equal(mock_sram.mem[17'h00200], 8'h42);

        spi_read_at(common_pkg::wb_ram_addr(17'h00200), dout);
        $display("[%t]   RAM[$0200] (SPI read) = $%02x (expect $42)", $time, dout);
        `assert_equal(dout, 8'h42);

        $display("[%t] END soft-6502 smoke test", $time);
    endtask

    `TB_INIT
endmodule
