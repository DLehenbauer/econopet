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

// Wishbone peripheral that receives the state of the USB keyboard
// from the MCU and exposes it as a 10x8 matrix.
module keyboard(
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_clock_i,
    input  logic [WB_ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [   DATA_WIDTH-1:0] wb_data_i,
    output logic [   DATA_WIDTH-1:0] wb_data_o,
    input  logic                     wb_we_i,
    input  logic                     wb_cycle_i,
    input  logic                     wb_strobe_i,
    output logic                     wb_stall_o,
    output logic                     wb_ack_o,
    input  logic                     wb_sel_i,      // Asserted when selected by 'wb_addr_i'

    output logic [KBD_COL_COUNT-1:0][KBD_ROW_COUNT-1:0] usb_kbd_o
);
    initial begin
        // PET keyboard matrix is all 1's when no keys are pressed.
        usb_kbd_o = '1;
    end
    
    // Wishbone address bits [3:0] select the column of the keyboard matrix.
    wire [KBD_COL_WIDTH-1:0] wb_rs = wb_addr_i[KBD_COL_WIDTH-1:0];

    // This peripheral always completes WB operations in a single cycle.
    assign wb_stall_o = 1'b0;

    always_ff @(posedge wb_clock_i) begin
        wb_ack_o <= '0;

        if (wb_sel_i && wb_cycle_i && wb_strobe_i) begin
            if (wb_we_i) begin
                usb_kbd_o[wb_rs] <= wb_data_i;
            end
            wb_data_o <= usb_kbd_o[wb_rs];
            wb_ack_o  <= 1'b1;
        end
    end
endmodule
