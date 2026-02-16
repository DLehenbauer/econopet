// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

/**
 * Wishbone peripheral multiplexer.
 *
 * Multiplexes responses from multiple Wishbone peripherals back to the controller.
 *
 * @param COUNT Number of Wishbone peripherals
 */
module wbp_mux #(
    parameter COUNT = 2   // Number of Wishbone peripherals
) (
    // Wishbone Bus
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    output logic [   DATA_WIDTH-1:0] wb_din_o,
    output logic                     wb_stall_o,
    output logic                     wb_ack_o,

    // Wishbone Peripherals
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic [COUNT-1:0][   DATA_WIDTH-1:0] wbp_din_i,     // Data bus
    input  logic [COUNT-1:0]                    wbp_stall_i,   // Stall
    input  logic [COUNT-1:0]                    wbp_ack_i,     // Acknowledge

    // Control signals
    input logic [COUNT-1:0] wbp_sel_i   // Select peripheral
);
    // Binary index corresponding to the one-hot position
    logic [$clog2(COUNT)-1:0] sel_index;

    // Convert the one-hot select signal to binary index
    always_comb begin
        sel_index = '0;
        
        for (int i = 0; i < COUNT; i++) begin
            if (wbp_sel_i[i]) begin
                sel_index = i[$bits(sel_index)-1:0];
            end
        end
    end

    always_comb begin
        wb_din_o   = wbp_din_i[sel_index];
        wb_stall_o = wbp_stall_i[sel_index];
        wb_ack_o   = wbp_ack_i[sel_index];
    end
endmodule
