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
 * @param CONTROLLER_COUNT Number of Wishbone controllers
 */
module wb_mux #(
    parameter COUNT = 2   // Number of Wishbone peripherals
) (
    input logic wb_clock_i,  // Clock

    // Wishbone Bus
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_cycle_i,
    input  logic                     wb_strobe_i,
    input  logic [WB_ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [   DATA_WIDTH-1:0] wb_data_i,
    output logic [   DATA_WIDTH-1:0] wb_data_o,
    input  logic                     wb_we_i,
    output logic                     wb_stall_o,
    output logic                     wb_ack_o,

    // Wishbone Peripherals
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    output logic [COUNT-1:0]                    wbp_cycle_o,   // Cycle valid
    output logic [COUNT-1:0]                    wbp_strobe_o,  // Strobe
    output logic [COUNT-1:0][WB_ADDR_WIDTH-1:0] wbp_addr_o,    // Address bus
    input  logic [COUNT-1:0][   DATA_WIDTH-1:0] wbp_data_i,    // Data bus
    output logic [COUNT-1:0][   DATA_WIDTH-1:0] wbp_data_o,    // Data bus
    output logic [COUNT-1:0]                    wbp_we_o,      // Write enable
    input  logic [COUNT-1:0]                    wbp_stall_i,   // Stall
    input  logic [COUNT-1:0]                    wbp_ack_i,     // Acknowledge

    // Control signals
    input logic [$clog2(COUNT)-1:0] wbp_sel_i   // Select peripheral
);
    always_comb begin
        wbp_cycle_o[wbp_sel_i] = wb_cycle_i;
        wbp_strobe_o[wbp_sel_i] = wb_strobe_i;
        wbp_addr_o[wbp_sel_i] = wb_addr_i;
        wbp_data_o[wbp_sel_i] = wb_data_i;
        wb_data_o = wbp_data_i[wbp_sel_i];
        wbp_we_o[wbp_sel_i] = wb_we_i;
        wb_stall_o = wbp_stall_i[wbp_sel_i];
        wb_ack_o = wbp_ack_i[wbp_sel_i];
    end
endmodule
