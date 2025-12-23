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

import common_pkg::*;

module bram #(
    parameter DATA_DEPTH,
    parameter ADDR_WIDTH = $clog2(DATA_DEPTH)
) (
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_clock_i,
    input  logic [WB_ADDR_WIDTH-1:0] wbp_addr_i,
    input  logic [   DATA_WIDTH-1:0] wbp_data_i,
    output logic [   DATA_WIDTH-1:0] wbp_data_o,
    input  logic                     wbp_we_i,
    input  logic                     wbp_cycle_i,
    input  logic                     wbp_strobe_i,
    output logic                     wbp_stall_o,
    output logic                     wbp_ack_o,
    input  logic                     wbp_sel_i       // Asserted when selected by 'wbp_addr_i'
);
    (* syn_ramstyle = "block_ram" *) reg [DATA_WIDTH-1:0] mem[DATA_DEPTH-1:0];

    initial begin
        integer i;

        wbp_ack_o = '0;

        // Per T8 datasheet: block RAM content is random and undefined if not initialized.
        for (i = 0; i < DATA_DEPTH; i = i + 1) begin
            mem[i] = '0;
        end
    end

    // This peripheral always completes WB operations in a single cycle.
    assign wbp_stall_o = 1'b0;

    // Extract the relevant address bits from the full Wishbone address
    wire [ADDR_WIDTH-1:0] mem_addr = wbp_addr_i[ADDR_WIDTH-1:0];

    always_ff @(posedge wb_clock_i) begin
        wbp_ack_o <= '0;

        if (wbp_sel_i && wbp_cycle_i && wbp_strobe_i) begin
            wbp_data_o <= mem[mem_addr];
            if (wbp_we_i) begin
                mem[mem_addr] <= wbp_data_i;
            end
            wbp_ack_o <= 1'b1;
        end
    end
endmodule
