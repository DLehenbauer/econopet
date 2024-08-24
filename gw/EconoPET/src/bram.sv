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

`include "./src/common_pkg.svh"

import common_pkg::*;

module bram #(
    parameter DATA_DEPTH = 512,
    parameter ADDR_WIDTH = $clog2(DATA_DEPTH - 1)
) (
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                  wb_clock_i,
    input  logic [ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [DATA_WIDTH-1:0] wb_data_i,
    output logic [DATA_WIDTH-1:0] wb_data_o,
    input  logic                  wb_we_i,
    input  logic                  wb_cycle_i,
    input  logic                  wb_strobe_i,
    output logic                  wb_stall_o,
    output logic                  wb_ack_o
);
    logic [DATA_WIDTH-1:0] mem[(2 ** ADDR_WIDTH)-1:0];

    initial begin
        integer i;

        wb_ack_o   = '0;
        wb_stall_o = '0;

        // Per T8 datasheet: block RAM content is random and undefined if not initialized.
        for (i = 0; i < DATA_DEPTH; i = i + 1) begin
            mem[i] = '0;
        end
    end

    always_ff @(posedge wb_clock_i) begin
        if (wb_cycle_i && wb_strobe_i) begin
            wb_data_o <= mem[wb_addr_i];
            if (wb_we_i) begin
                mem[wb_addr_i] <= wb_data_i;
            end
            wb_ack_o <= 1'b1;
        end else begin
            wb_ack_o <= '0;
        end
    end
endmodule
