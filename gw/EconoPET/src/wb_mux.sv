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

/**
 * Wishbone Multiplexer (One Bus -> Many Peripherals)
 *
 * @param COUNT Number of Wishbone controllers
 */
module wb_mux #(
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
        sel_index = ($bits(sel_index-1))'(0);
        
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
