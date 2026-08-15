// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Verifies the board ~IRQ -> soft 6809 interrupt path end-to-end through
// 'top': cpu_irq_n_i pin -> active-high cpu_irq_i -> 2FF synchronizer ->
// mc6809i nIRQ -> $FFF8 vector fetch -> handler executes -> RTI, and that
// releasing ~IRQ stops further interrupts (no stuck-IRQ storm).
//
// Waterloo's keyboard is serviced exclusively from the PIA1 CB1 (vertical
// retrace) IRQ, so this path is required for any SuperPET keyboard input.
// Uses a tiny synthetic program (no ROM images required):
//
//   $F000: LDS #$0100      ; stack for the IRQ frame
//   $F004: ANDCC #$EF      ; unmask IRQ
//   $F006: JMP $F006       ; idle
//   $F100: INC $80         ; IRQ handler: count invocations
//          RTI
module superpet_irq_tb;

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

    // Board ~IRQ net: open-drain, pulled up; testbench stands in for the
    // PIA/VIA interrupt outputs.
    bit irq_n = 1'b1;

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

        .cpu_irq_n_i(irq_n),

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

    // Count $FFF8 (IRQ) vector fetches observed on the bus.
    int unsigned vector_fetches = 0;
    logic prev_fff8 = 0;
    always_ff @(posedge sys_clock) begin
        logic now_fff8;
        now_fff8 = top.main.cpu6809_be && (top.main.active_cpu_addr == 16'hFFF8) && !top.main.active_cpu_we;
        if (now_fff8 && !prev_fff8) vector_fetches <= vector_fetches + 1;
        prev_fff8 <= now_fff8;
    end

    task static run;
        int unsigned count_after_burst;

        $display("[%t] BEGIN SuperPET 6809 IRQ delivery test", $time);
        spi1_driver.reset;

        // Program (see header). ROM region $F000+ lives at SRAM $0F000.
        mock_sram.mem[17'h0F000] = 8'h10;   // LDS #$0100
        mock_sram.mem[17'h0F001] = 8'hCE;
        mock_sram.mem[17'h0F002] = 8'h01;
        mock_sram.mem[17'h0F003] = 8'h00;
        mock_sram.mem[17'h0F004] = 8'h1C;   // ANDCC #$EF
        mock_sram.mem[17'h0F005] = 8'hEF;
        mock_sram.mem[17'h0F006] = 8'h7E;   // JMP $F006
        mock_sram.mem[17'h0F007] = 8'hF0;
        mock_sram.mem[17'h0F008] = 8'h06;

        mock_sram.mem[17'h0F100] = 8'h7C;   // INC $0080 (extended)
        mock_sram.mem[17'h0F101] = 8'h00;
        mock_sram.mem[17'h0F102] = 8'h80;
        mock_sram.mem[17'h0F103] = 8'h3B;   // RTI

        mock_sram.mem[17'h0FFF8] = 8'hF1;   // IRQ vector -> $F100
        mock_sram.mem[17'h0FFF9] = 8'h00;
        mock_sram.mem[17'h0FFFE] = 8'hF0;   // RESET vector -> $F000
        mock_sram.mem[17'h0FFFF] = 8'h00;

        mock_sram.mem[17'h00080] = 8'h00;   // IRQ handler invocation counter

        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6809));

        $display("[%t]   Releasing reset via REG_CPU", $time);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0000);

        // Let the program reach its idle loop (~50 bus cycles at 1us each).
        #200000;

        // 1. No IRQ asserted -> handler must never have run (also catches an
        //    'x' leaking into the synchronizer from the ~IRQ input).
        `assert_equal(mock_sram.mem[17'h00080], 8'h00);
        `assert_equal(vector_fetches, 0);

        // 2. Assert ~IRQ (level, as the PIAs would): handler must run.
        $display("[%t]   Asserting ~IRQ", $time);
        irq_n = 1'b0;
        #500000;
        $display("[%t]   handler count=%0d, vector fetches=%0d",
            $time, mock_sram.mem[17'h00080], vector_fetches);
        assert(mock_sram.mem[17'h00080] >= 8'h01)
            else $fatal(1, "IRQ handler never ran (count=%0d)", mock_sram.mem[17'h00080]);
        assert(vector_fetches >= 1)
            else $fatal(1, "No $FFF8 vector fetch observed");

        // 3. While ~IRQ stays asserted, a level interrupt re-enters after
        //    each RTI -- the count keeps climbing.
        count_after_burst = 32'(mock_sram.mem[17'h00080]);
        #500000;
        assert(32'(mock_sram.mem[17'h00080]) > count_after_burst)
            else $fatal(1, "Level ~IRQ did not re-enter handler");

        // 4. Release ~IRQ: interrupts must stop (count settles).
        $display("[%t]   Releasing ~IRQ", $time);
        irq_n = 1'b1;
        #100000;    // drain any in-flight handler
        count_after_burst = 32'(mock_sram.mem[17'h00080]);
        #500000;
        `assert_equal(32'(mock_sram.mem[17'h00080]), count_after_burst);

        $display("[%t] END SuperPET 6809 IRQ delivery test", $time);
    endtask

    task static spi_write_at (
        input logic [WB_ADDR_WIDTH-1:0] addr_i,
        input logic [   DATA_WIDTH-1:0] data_i
    );
        spi1_driver.write_at(addr_i, data_i);
    endtask

    `TB_INIT
endmodule
