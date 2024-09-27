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

module wb_mux #(
    parameter CC = 2,  // Number of Wishbone controllers
    parameter PC = 2   // Number of Wishbone peripherals
) (
    input logic wb_clock_i,  // Clock

    // Wishbone B4 controllers
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic [CC-1:0]                    wbc_cycle_i,   // Cycle request
    input  logic [CC-1:0]                    wbc_strobe_i,  // Strobe
    input  logic [CC-1:0][WB_ADDR_WIDTH-1:0] wbc_addr_i,    // Address bus
    input  logic [CC-1:0][   DATA_WIDTH-1:0] wbc_data_i,    // Data bus
    output logic [CC-1:0][   DATA_WIDTH-1:0] wbc_data_o,    // Data bus
    input  logic [CC-1:0]                    wbc_we_i,      // Write enable
    output logic [CC-1:0]                    wbc_stall_o,   // Stall
    output logic [CC-1:0]                    wbc_ack_o,     // Acknowledge

    // Wishbone B4 peripherals
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    output logic [PC-1:0]                    wbp_cycle_o,   // Cycle valid
    output logic [PC-1:0]                    wbp_strobe_o,  // Strobe
    output logic [PC-1:0][WB_ADDR_WIDTH-1:0] wbp_addr_o,    // Address bus
    input  logic [PC-1:0][   DATA_WIDTH-1:0] wbp_data_i,    // Data bus
    output logic [PC-1:0][   DATA_WIDTH-1:0] wbp_data_o,    // Data bus
    output logic [PC-1:0]                    wbp_we_o,      // Write enable
    input  logic [PC-1:0]                    wbp_stall_i,   // Stall
    input  logic [PC-1:0]                    wbp_ack_i,     // Acknowledge

    // Control signals
    input logic [$clog2(CC)-1:0] wbc_sel_i,  // Select controller
    input logic [$clog2(PC)-1:0] wbp_sel_i   // Select peripheral
);
    always_comb begin
        wbp_cycle_o[wbp_sel_i] = wbc_cycle_i[wbc_sel_i];
        wbp_strobe_o[wbp_sel_i] = wbc_strobe_i[wbc_sel_i];
        wbp_addr_o[wbp_sel_i] = wbc_addr_i[wbc_sel_i];
        wbp_data_o[wbp_sel_i] = wbc_data_i[wbc_sel_i];
        wbc_data_o[wbc_sel_i] = wbp_data_i[wbp_sel_i];
        wbp_we_o[wbp_sel_i] = wbc_we_i[wbc_sel_i];
        wbc_stall_o[wbc_sel_i] = wbp_stall_i[wbp_sel_i];
        wbc_ack_o[wbc_sel_i] = wbp_ack_i[wbp_sel_i];
    end
endmodule
