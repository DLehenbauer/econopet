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

module mock_system (
    output logic cpu_clock_o,
    output logic cpu_reset_n_o,
    output logic cpu_ready_o
);
    // Clock
    bit sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) sys_clock_gen (.clock_o(sys_clock));
    initial sys_clock_gen.start;

    // Bus
    logic [CPU_ADDR_WIDTH-1:0] bus_addr;
    logic [DATA_WIDTH-1:0]     bus_data;
    logic bus_we_n;

    // CPU
    logic cpu_be;

    // Convert wire-or with pullup to a single wire.
    bit   manual_reset_n = 1'b1;
    logic top_reset_n;
    logic top_reset_n_oe;
    assign cpu_reset_n_o = top_reset_n_oe ? top_reset_n : manual_reset_n;

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

    // IO
    logic io_oe_n;
    logic pia1_cs_n;
    logic pia2_cs_n;
    logic via_cs_n;

    // SPI
    logic spi_sck;
    logic spi_cs_n;
    logic spi_pico;
    logic spi_poci;
    logic spi_stall;
    logic [7:0] spi_rx_data;

    // Video
    logic video_graphics = 1'b0;

    top top (
        .sys_clock_i(sys_clock),

        .cpu_be_o(cpu_be),
        .cpu_ready_o(cpu_ready_o),
        .cpu_reset_n_o(top_reset_n),
        .cpu_reset_n_oe(top_reset_n_oe),
        .cpu_clock_o(cpu_clock_o),
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

        .io_oe_n_o(io_oe_n),
        .pia1_cs_n_o(pia1_cs_n),
        .pia2_cs_n_o(pia2_cs_n),
        .via_cs_n_o(via_cs_n),

        .spi0_cs_ni (spi_cs_n),
        .spi0_sck_i (spi_sck),
        .spi0_sd_i  (spi_pico),
        .spi0_sd_o  (spi_poci),
        .spi_stall_o(spi_stall),

        .graphic_i(video_graphics)
    );

    logic [CPU_ADDR_WIDTH-1:0] cpu_addr;
    logic [DATA_WIDTH-1:0] cpu_data;
    logic cpu_we_n;

    mock_cpu mock_cpu(
        .sys_clock_i(sys_clock),
        .cpu_clock_i(cpu_clock_o),
        .reset_n_i(cpu_reset_n_o),
        .addr_o(cpu_addr),
        .data_i(bus_data),
        .data_o(cpu_data),
        .we_n_o(cpu_we_n),
        .irq_n_i(1'b1),
        .nmi_n_i(1'b1),
        .ready_i(cpu_ready_o)
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
        .clock_i(sys_clock),
        .ram_addr_i(ram_addr),
        .ram_data_i(bus_data),
        .ram_data_o(ram_data),
        .ram_we_n_i(ram_we_n_o),
        .ram_oe_n_i(ram_oe_n_o)
    );

    mock_bus mock_bus (
        .clock_i(sys_clock),

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

        // Incoming bus outputs from 'mock_io' module
        .io_data_i(8'h10),
        .io_oe_n_i(io_oe_n),

        .bus_addr_o(bus_addr),
        .bus_data_o(bus_data),
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

    task static rom_init;
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
    endtask

    task static ram_fill(
        input logic [RAM_ADDR_WIDTH-1:0] start_addr,
        input logic [RAM_ADDR_WIDTH-1:0] end_addr,
        input logic [DATA_WIDTH-1:0] data
    );
        mock_ram.fill(start_addr, end_addr, data);
    endtask

    task static spi_read (
        output logic [DATA_WIDTH-1:0] data_o
    );
        spi1_driver.read_next;
        data_o = spi_rx_data;
    endtask

    task static spi_read_at (
        input  logic [WB_ADDR_WIDTH-1:0] addr_i,
        output logic [   DATA_WIDTH-1:0] data_o
    );
        spi1_driver.read_at(addr_i);            // Seek for next read
        spi_read(data_o);                       // Read data
    endtask

    task static spi_write_at (
        input logic [WB_ADDR_WIDTH-1:0] addr_i,
        input logic [   DATA_WIDTH-1:0] data_i
    );
        spi1_driver.write_at(addr_i, data_i);
    endtask

    task static cpu_start;
        mock_cpu.start;
    endtask

    task static cpu_stop;
        mock_cpu.stop;
    endtask

    task static cpu_reset;
        manual_reset_n = 1'b0;
        
        // Verilog-6502 requires two cycles to reset.
        @(cpu_clock_o);
        @(cpu_clock_o);
        
        manual_reset_n = 1'b1;
    endtask

    task static cpu_read (
        input  logic [CPU_ADDR_WIDTH-1:0] addr_i,
        output logic [    DATA_WIDTH-1:0] data_o
    );
        mock_cpu.read(addr_i, data_o);
    endtask

    task static cpu_write (
        input logic [CPU_ADDR_WIDTH-1:0] addr_i,
        input logic [    DATA_WIDTH-1:0] data_i
    );
        mock_cpu.write(addr_i, data_i);
    endtask

    task static init;
        spi1_driver.reset;
    endtask
endmodule
