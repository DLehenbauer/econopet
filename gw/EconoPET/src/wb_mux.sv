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
    parameter CC = 1,
    parameter PC = 1
) (
    // Wishbone Controllers
    output logic [WB_ADDR_WIDTH-1:0][CC-1:0] wbc_addr_o,
    output logic [   DATA_WIDTH-1:0][CC-1:0] wbc_data_o,
    input  logic [   DATA_WIDTH-1:0][CC-1:0] wbc_data_i,
    output logic [CC-1:0] wbc_we_o,
    output logic [CC-1:0] wbc_cycle_o,
    output logic [CC-1:0] wbc_strobe_o,
    input  logic [CC-1:0] wbc_stall_i,
    input  logic [CC-1:0] wbc_ack_i
);
endmodule
