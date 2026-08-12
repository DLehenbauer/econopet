// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Boots the real SuperPET Waterloo ROM set (not a synthetic test program --
// see superpet_top_tb.sv for that) against the mc6809 core integration, to
// check whether actual SuperPET software gets anywhere. Same non-CPU wiring
// as superpet_top_tb.sv (mock_sram + mock_bus + SPI1, no mock 6502).
//
// ROM images (place in ECONOPET_ROMS_DIR):
//   waterloo-a000-bfff.970018-12.bin  (8KB,  $A000-$BFFF)
//   waterloo-c000-dfff.970019-12.bin  (8KB,  $C000-$DFFF)
//   waterloo-e000-ffff-970034-12.bin  (8KB,  $E000-$FFFF, incl. reset vector)
// Source: https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/
module superpet_waterloo_tb;
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

    task static spi_write_at (
        input logic [WB_ADDR_WIDTH-1:0] addr_i,
        input logic [   DATA_WIDTH-1:0] data_i
    );
        spi1_driver.write_at(addr_i, data_i);
    endtask

    // Best-effort PET screen-code -> printable ASCII for eyeballing captured
    // screen RAM. 0-31 -> '@'/'A'-'Z'/symbols (+64), 32-63 -> direct (space,
    // digits, punctuation), everything else (graphics/reverse video) -> '.'.
    function automatic byte screen_to_ascii(input byte code);
        if (code < 8'd32) return byte'(code + 8'd64);
        if (code < 8'd64) return code;
        return ".";
    endfunction

    task static dump_screen_row(input int row);
        string line;
        int base;
        line = "";
        base = row * 80;
        for (int col = 0; col < 80; col++) begin
            line = { line, $sformatf("%c", screen_to_ascii(mock_sram.mem[17'(base + col)])) };
        end
        $display("[%t]   SCREEN[%0d]: %s", $time, row, line);
    endtask

    task static run;
        $display("[%t] BEGIN SuperPET Waterloo boot test", $time);
        spi1_driver.reset;

        $display("[%t]   Loading Waterloo ROMs", $time);
        mock_sram.load_rom(17'h0A000, "waterloo-a000-bfff.970018-12.bin");
        mock_sram.load_rom(17'h0C000, "waterloo-c000-dfff.970019-12.bin");
        mock_sram.load_rom(17'h0E000, "waterloo-e000-ffff-970034-12.bin");

        // Clear screen RAM.
        mock_sram.fill(17'h08000, 17'h087CF, 8'h20);  // 80x25 screen, space-filled

        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6809));

        $display("[%t]   Releasing reset via REG_CPU", $time);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0000);

        // Budget generously for a real OS boot sequence and report progress
        // periodically so a hang is visible rather than one long silence.
        for (int i = 0; i < 800; i++) begin
            #250000;
            if (i % 20 == 0) $display("[%t]   ...still running (PC-ish addr=$%h, E=%b)",
                $time, top.main.mc6809_addr, top.main.cpu6809_e);
        end

        $display("[%t]   Captured screen RAM (all 25 rows):", $time);
        for (int row = 0; row < 25; row++) dump_screen_row(row);

        $display("[%t] END SuperPET Waterloo boot test", $time);
    endtask

    `TB_INIT
endmodule
