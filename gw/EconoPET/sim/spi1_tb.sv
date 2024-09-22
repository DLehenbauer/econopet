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

module spi1_tb;
    bit clock;

    clock_gen #(SYS_CLOCK_MHZ) fpga_clk (.clock_o(clock));

    initial fpga_clk.start;

    logic spi_sck;
    logic spi_cs_n;
    logic spi_pico;
    logic spi_poci;
    logic spi_stall;

    spi1_driver spi1_driver (
        .clock_i(clock),
        .spi_sck_o(spi_sck),
        .spi_cs_no(spi_cs_n),
        .spi_pico_o(spi_pico),
        .spi_poci_i(spi_poci),
        .spi_stall_i(spi_stall)
    );

    logic [WB_ADDR_WIDTH-1:0] addr;
    logic [   DATA_WIDTH-1:0] rd_data;
    logic [   DATA_WIDTH-1:0] wr_data;
    logic                     we;
    logic                     cycle;
    logic                     stall = 1'b0;
    logic                     ack   = 1'b0;

    spi1_controller spi1 (
        .wb_clock_i(clock),
        .wb_addr_o(addr),
        .wb_data_i(rd_data),
        .wb_data_o(wr_data),
        .wb_we_o(we),
        .wb_cycle_o(cycle),
        .wb_stall_i(stall),
        .wb_ack_i(ack),

        .spi_sck_i(spi_sck),
        .spi_cs_ni(spi_cs_n),
        .spi_sd_i (spi_pico),
        .spi_sd_o (spi_poci),

        .spi_stall_o(spi_stall)
    );

    logic [WB_ADDR_WIDTH-1:0] expected_addr;
    logic                     expected_we;
    logic [   DATA_WIDTH-1:0] expected_data;

    task set_expected(input logic [WB_ADDR_WIDTH-1:0] addr_i,
                      input logic we_i,
                      input logic [DATA_WIDTH-1:0] data_i = 8'hxx);
        expected_addr <= addr_i;
        expected_data <= data_i;
        expected_we   <= we_i;
    endtask

    task write_at(
        input logic [WB_ADDR_WIDTH-1:0] addr_i,
        input logic [DATA_WIDTH-1:0] data_i
    );
        set_expected(/* addr: */ addr_i, /* we: */ 1'b1, /* data: */ data_i);
        spi1_driver.write_at(addr_i, data_i);
    endtask

    task read_at(
        input logic [WB_ADDR_WIDTH-1:0] addr_i
    );
        set_expected(/* addr: */ addr_i, /* we: */ '0);
        spi1_driver.read_at(addr_i);
    endtask

    task read_next();
        set_expected(/* addr: */ expected_addr + 1'b1, /* we: */ '0);
        spi1_driver.read_next();
    endtask

    always @(posedge spi_cs_n) begin
        @(posedge clock)  // 2FF stage 1
        @(posedge clock)  // 2FF stage 2
        @(posedge clock)  // Edge detect

        #1;

        `assert_equal(spi_stall, '0);
    end

    always @(negedge spi_cs_n) begin
        @(posedge clock)  // 2FF stage 1
        @(posedge clock)  // 2FF stage 2
        @(posedge clock)  // Edge detect

        #1;

        // An in-progress cycle is terminated if 'spi_cs_n' is deasserted.
        `assert_equal(cycle, '0);
    end

    always @(posedge ack) begin
        // Setting ack clears cycle on next clock edge.
        @(posedge clock) begin
            `assert_equal(cycle, '1);
            ack <= '0;
        end

        @(posedge clock) begin
            `assert_equal(cycle, '0);
            `assert_equal(ack, '0);
        end
    end

    always @(posedge cycle) begin
        $display("[%t]        (stall=%b, cycle=%b, ack=%b, addr=%x, we=%b, wr_data=%x)", $time,
                 spi_stall, cycle, ack, addr, we, wr_data);

        `assert_equal(addr, expected_addr);
        `assert_equal(we, expected_we);
        if (we) `assert_equal(wr_data, expected_data);

        @(posedge clock) ack <= 1'b1;
        #1 $display("[%t]        (stall=%b, cycle=%b, ack=%b)", $time, spi_stall, cycle, ack);
        @(negedge spi_stall);
        $display("[%t]        (stall=%b, cycle=%b, ack=%b)", $time, spi_stall, cycle, ack);
        ack <= 1'b0;
    end

    task run;
        spi1_driver.reset();

        write_at(20'h00000, 8'h00);
        read_next(8'h01);
        read_next(8'h01);
        read_next(8'h01);
        read_next(8'h01);
        read_next(8'h01);
        write_at(20'hFFFFF, 8'h00);
    endtask
endmodule
