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

module wb_driver #(
    parameter DATA_WIDTH = 8,
    parameter ADDR_WIDTH = 10
) (
    input logic                   wb_clock_i,

    output logic [ADDR_WIDTH-1:0] wb_addr_o,
    input  logic [DATA_WIDTH-1:0] wb_data_i,
    output logic [DATA_WIDTH-1:0] wb_data_o,
    output logic                  wb_we_o,
    output logic                  wb_cycle_o,
    output logic                  wb_strobe_o,
    input  logic                  wb_stall_i,
    input  logic                  wb_ack_i
);
    task reset;
        wb_cycle_o  = '0;
        wb_strobe_o = '0;
        @(posedge wb_clock_i);
    endtask

    task start_bus_cycle;
        // Driver does not yet handle pipelined requests
        `assert_equal(wb_strobe_o, '0);

        wb_cycle_o  <= 1'b1;
        wb_strobe_o <= 1'b1;

        @(posedge wb_clock_i) wb_strobe_o <= '0;
    endtask

    task wait_for_ack;
        `assert_equal(wb_cycle_o, 1'b1);
        while (!wb_ack_i) @(posedge wb_clock_i);
    endtask

    task end_bus_cycle;
        `assert_equal(wb_cycle_o, 1'b1);
        `assert_equal(wb_strobe_o, '0);
        wb_cycle_o <= '0;
    endtask

    task write(
        input logic [ADDR_WIDTH-1:0] addr_i,
        input logic [DATA_WIDTH-1:0] data_i
    );
        wb_addr_o = addr_i;
        wb_data_o = data_i;
        wb_we_o   = 1'b1;

        start_bus_cycle;
        wait_for_ack;
        end_bus_cycle;
    endtask

    task read(
        input  logic [ADDR_WIDTH-1:0] addr_i,
        output logic [DATA_WIDTH-1:0] data_o,
        output logic                  ack_o
    );
        wb_addr_o = addr_i;
        wb_data_o = 8'hxx;
        wb_we_o   = '0;

        start_bus_cycle;
        wait_for_ack;
        data_o = wb_data_i;
        end_bus_cycle;
    endtask
endmodule
