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

module top_tb #(
    parameter integer unsigned CLK_MHZ = 64,
    parameter integer unsigned DATA_WIDTH = 8,
    parameter integer unsigned CPU_ADDR_WIDTH = 16,
    parameter integer unsigned RAM_ADDR_WIDTH = 17
);
    // Clock
    bit clock;
    clock_gen #(CLK_MHZ) fpga_clock (.clock_o(clock));
    initial fpga_clock.start;

    // Bus
    logic [CPU_ADDR_WIDTH-1:0] bus_addr;
    logic [DATA_WIDTH-1:0]     bus_data;
    logic bus_we_n;

    // CPU
    logic cpu_be;
    logic cpu_ready;
    logic cpu_clock;
    logic [CPU_ADDR_WIDTH-1:0] top_addr;
    logic [CPU_ADDR_WIDTH-1:0] top_addr_oe;
    logic [DATA_WIDTH-1:0] top_data;
    logic [DATA_WIDTH-1:0] top_data_oe;
    logic top_we_n;
    logic top_we_n_oe;

    // RAM
    logic ram_addr_a10_o;
    logic ram_addr_a11_o;
    logic ram_addr_a15_o;
    logic ram_addr_a16_o;
    logic ram_oe_n_o;
    logic ram_we_n_o;

    // SPI
    logic spi_sck;
    logic spi_cs_n;
    logic spi_pico;
    logic spi_poci;
    logic spi_stall;
    logic [7:0] spi_rx_data;

    top top (
        .clock_i(clock),

        .cpu_be_o(cpu_be),
        .cpu_ready_o(cpu_ready),
        .cpu_clock_o(cpu_clock),
        .cpu_addr_i (bus_addr),
        .cpu_addr_o (top_addr),
        .cpu_addr_oe(top_addr_oe),
        .cpu_data_i (bus_data),
        .cpu_data_o (top_data),
        .cpu_data_oe(top_data_oe),
        .cpu_we_n_i (bus_we_n),
        .cpu_we_n_o (top_we_n),
        .cpu_we_n_oe(top_we_n_oe),

        .ram_addr_a10_o(ram_addr_a10_o),
        .ram_addr_a11_o(ram_addr_a11_o),
        .ram_addr_a15_o(ram_addr_a15_o),
        .ram_addr_a16_o(ram_addr_a16_o),
        .ram_oe_n_o(ram_oe_n_o),
        .ram_we_n_o(ram_we_n_o),

        .spi1_cs_ni (spi_cs_n),
        .spi1_sck_i (spi_sck),
        .spi1_sd_i  (spi_pico),
        .spi1_sd_o  (spi_poci),
        .spi_stall_o(spi_stall)
    );

    logic [CPU_ADDR_WIDTH-1:0] cpu_addr;
    logic [DATA_WIDTH-1:0] cpu_data;
    logic cpu_we_n;
    logic cpu_reset_n = 0;

    mock_cpu mock_cpu(
        .sys_clock_i(clock),
        .cpu_clock_i(cpu_clock),
        .reset_n_i(cpu_reset_n),    // TODO: Use generated 'cpu_reset_n' from top module.
        .addr_o(cpu_addr),
        .data_i(bus_data),
        .data_o(cpu_data),
        .we_n_o(cpu_we_n),
        .irq_n_i(1'b1),
        .nmi_n_i(1'b1),
        .ready_i(cpu_ready)
    );

    logic [DATA_WIDTH-1:0] ram_data;

    wire [RAM_ADDR_WIDTH-1:0] ram_addr = {
        ram_addr_a16_o,
        ram_addr_a15_o,
        bus_addr[14:12],
        ram_addr_a11_o,
        ram_addr_a10_o,
        bus_addr[9:0]
    };

    mock_ram mock_ram (
        .clock_i(clock),
        .ram_addr_i(ram_addr),
        .ram_data_i(bus_data),
        .ram_data_o(ram_data),
        .ram_we_n_i(ram_we_n_o),
        .ram_oe_n_i(ram_oe_n_o)
    );

    mock_bus mock_bus (
        .clock_i(clock),

        // Incoming bus outputs from FPGA 'top' module
        .top_addr_i(top_addr),
        .top_addr_oe_i(top_addr_oe[0]),
        .top_data_i(top_data),
        .top_data_oe_i(top_data_oe[0]),
        .top_we_n_i(top_we_n),
        .top_we_n_oe_i(top_we_n_oe),

        // Incoming bus outputs from 'mock_cpu' module
        .cpu_be_i(cpu_be),
        .cpu_addr_i(cpu_addr),
        .cpu_data_i(cpu_data),
        .cpu_we_n_i(cpu_we_n),

        // Incoming bus outputs from 'mock_ram' module
        .ram_data_i(ram_data),
        .ram_oe_n_o(ram_oe_n_o),
        .ram_we_n_o(ram_we_n_o),

        .bus_addr_o(bus_addr),
        .bus_data_o(bus_data),
        .bus_we_n_o(bus_we_n)
    );

    spi1_driver spi1_driver (
        .clock_i(clock),
        .spi_sck_o(spi_sck),
        .spi_cs_no(spi_cs_n),
        .spi_pico_o(spi_pico),
        .spi_poci_i(spi_poci),
        .spi_stall_i(spi_stall),
        .spi_data_o(spi_rx_data)
    );

    task static test_rw(logic [16:0] addr_i, logic [7:0] data_i);
        spi1_driver.write_at(addr_i, data_i);
        spi1_driver.read_at(addr_i);            // Seek for next read
        spi1_driver.read_next;                  // 'read_next' required to actually retrieve the data.
        `assert_equal(spi_rx_data, data_i);
    endtask

    task static run;
        // Load ROMs matching dissassembly at:
        // https://www.zimmers.net/anonftp/pub/cbm/src/pet/pet_rom4_disassembly.txt
        //
        // Memory map:
        // https://www.commodore.ca/manuals/pdfs/commodore_pet_memory_map.pdf
        //
        // Helpful 6502 opcode reference:
        // http://www.6502.org/tutorials/6502opcodes.html
        mock_ram.load_rom(16'h8800, "characters-2.901447-10.bin");
        mock_ram.load_rom(16'hb000, "basic-4-b000.901465-23.bin");
        mock_ram.load_rom(16'hc000, "basic-4-c000.901465-20.bin");
        mock_ram.load_rom(16'hd000, "basic-4-d000.901465-21.bin");
        mock_ram.load_rom(16'he000, "edit-4-80-b-60Hz.901474-03.bin");
        mock_ram.load_rom(16'hf000, "kernal-4.901465-22.bin");

        $display("[%t] BEGIN %m", $time);

        cpu_reset_n = 0;
        // Verilog-6502 requires two cycles to reset.
        @(posedge cpu_clock);
        @(posedge cpu_clock);
        cpu_reset_n = 1;

        spi1_driver.reset;

        test_rw(20'h0_4000, 8'h00);
        test_rw(20'h0_4000, 8'h01);

        #100000;

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
