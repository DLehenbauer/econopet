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
 * Wishbone Demultiplexer (Many Controllers -> One Bus)
 *
 * @param COUNT Number of Wishbone controllers
 */
module wb_demux #(
    parameter COUNT = 2  // Number of Wishbone controllers
) (
    input logic wb_clock_i,  // Clock

    // Wishbone controllers to demux
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic [COUNT-1:0]                    wbc_cycle_i,
    input  logic [COUNT-1:0]                    wbc_strobe_i,
    input  logic [COUNT-1:0][WB_ADDR_WIDTH-1:0] wbc_addr_i,
    input  logic [COUNT-1:0][   DATA_WIDTH-1:0] wbc_data_i,
    input  logic [COUNT-1:0]                    wbc_we_i,
    output logic [COUNT-1:0]                    wbc_stall_o,
    output logic [COUNT-1:0]                    wbc_ack_o,

    // Wishbone bus
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    output logic                     wb_cycle_o,
    output logic                     wb_strobe_o,
    output logic [WB_ADDR_WIDTH-1:0] wb_addr_o,
    output logic [   DATA_WIDTH-1:0] wb_data_o,
    output logic                     wb_we_o,
    input  logic                     wb_stall_i,
    input  logic                     wb_ack_i,

    // Control signals
    input logic [$clog2(COUNT)-1:0] wbc_sel, // Select controller
    input logic wb_en_i                      // (0 = stall new requests, 1 = admit new requests)
);
    always_comb begin
        // Peripheral receives inputs from the selected controller
        wb_cycle_o  = wbc_cycle_i[wbc_sel];        
        wb_addr_o   = wbc_addr_i[wbc_sel];
        wb_data_o   = wbc_data_i[wbc_sel];
        wb_we_o     = wbc_we_i[wbc_sel];

        // Deasserting 'wb_en_i' prevents new requests from being admitted by blocking the outgoing
        // strobe signal and returning a stall signal to the selected controller (below).
        wb_strobe_o = wb_en_i & wbc_strobe_i[wbc_sel];

        for (int i = 0; i < COUNT; i = i + 1) begin
            if (i[$bits(wbc_sel)-1:0] == wbc_sel) begin
                // Deliver stall and ack signals to selected controller.
                wbc_stall_o[i] = !wb_en_i || wb_stall_i;
                wbc_ack_o[i]   = wb_ack_i;
            end else begin
                // All other controls are stalled and do not receive ack for current cycle.
                // We assert stall even if there is no active request.
                wbc_stall_o[i] = 1'b1;
                wbc_ack_o[i]   = 1'b0;
            end
        end
    end
endmodule
