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

 module arbiter #(
    parameter integer unsigned DATA_WIDTH = 8,
    parameter integer unsigned WB_ADDR_WIDTH = 20,
    parameter integer unsigned CPU_ADDR_WIDTH = 16,
    parameter integer unsigned RAM_ADDR_WIDTH = 17
) (
    // Wishbone B4 controller
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_clock_i,
    input  logic                     wb_reset_i,

    // Controller
    input  logic                     wbc_cycle_i,
    input  logic                     wbc_strobe_i,
    output logic                     wbc_stall_o,
    output logic                     wbc_ack_o,

    // Peripheral
    output logic                     wbp_cycle_o,
    output logic                     wbp_strobe_o,
    input  logic                     wbp_stall_i,
    input  logic                     wbp_ack_i,

    input  logic                     grant_i
);
    assign wbc_ack_o    =  wbp_ack_i;
    assign wbc_stall_o  = ~grant_i | wbp_stall_i;
    assign wbp_cycle_o  =  grant_i & wbc_cycle_i;
    assign wbp_strobe_o =  grant_i & wbc_strobe_i;
endmodule
