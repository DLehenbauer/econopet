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
    parameter CLK_MHZ = 64
);
    bit clock;

    clock_gen#(CLK_MHZ) fpga_clock(
        .clock_o(clock)
    );

    initial fpga_clock.start;

    logic spi_sck;
    logic spi_cs_n;
    logic spi_pico;
    logic spi_poci;
    logic spi_stall;

    spi1_driver spi1(
        .clock_i(clock),
        .spi_sck_o(spi_sck),
        .spi_cs_no(spi_cs_n),
        .spi_pico_o(spi_pico),
        .spi_poci_i(spi_poci),
        .spi_stall_i(spi_stall)
    );

    top dut(
        .clock_i(clock),
        .spi1_cs_ni(spi_cs_n),
        .spi1_sck_i(spi_sck),
        .spi1_sd_i(spi_pico),
        .spi1_sd_o(spi_poci)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        spi1.reset;
        spi1.write_at(17'h00000, 8'h00);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
