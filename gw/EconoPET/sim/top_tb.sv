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

`timescale 1ns / 1ps
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

    // CPU
    logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i;
    logic [CPU_ADDR_WIDTH-1:0] cpu_addr_o;
    logic [CPU_ADDR_WIDTH-1:0] cpu_addr_oe;

    logic [DATA_WIDTH-1:0] cpu_data_i;
    logic [DATA_WIDTH-1:0] cpu_data_o;
    logic [DATA_WIDTH-1:0] cpu_data_oe;

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

    top dut (
        .clock_i(clock),

        .cpu_addr_i (cpu_addr_i),
        .cpu_addr_o (cpu_addr_o),
        .cpu_addr_oe(cpu_addr_oe),
        .cpu_data_i (cpu_data_i),
        .cpu_data_o (cpu_data_o),
        .cpu_data_oe(cpu_data_oe),

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

    wire [RAM_ADDR_WIDTH-1:0] ram_addr = {
        ram_addr_a16_o,
        ram_addr_a15_o,
        cpu_addr_o[14:12],
        ram_addr_a11_o,
        ram_addr_a10_o,
        cpu_addr_o[9:0]
    };

    mock_ram mock_ram (
        .ram_addr_i(ram_addr),
        .ram_data_i(cpu_data_o),
        .ram_data_o(cpu_data_i),
        .ram_we_n_i(ram_we_n_o),
        .ram_oe_n_i(ram_oe_n_o)
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
        $display("[%t] BEGIN %m", $time);

        spi1_driver.reset;

        test_rw(20'h0_0000, 8'h00);
        test_rw(20'h0_0000, 8'h01);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
