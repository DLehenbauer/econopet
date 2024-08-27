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

module ram(
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_clock_i,
    input  logic [WB_ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [   DATA_WIDTH-1:0] wb_data_i,    // Incoming data to write to RAM
    output logic [   DATA_WIDTH-1:0] wb_data_o,    // Outgoing data read from RAM
    input  logic                     wb_we_i,
    input  logic                     wb_cycle_i,
    input  logic                     wb_strobe_i,
    output logic                     wb_stall_o,
    output logic                     wb_ack_o,

    output logic                      ram_oe_o,
    output logic                      ram_we_o,
    output logic [RAM_ADDR_WIDTH-1:0] ram_addr_o,
    input  logic [    DATA_WIDTH-1:0] ram_data_i,
    output logic [    DATA_WIDTH-1:0] ram_data_o,
    output logic                      ram_data_oe
);
    wire wb_select = wb_addr_i[WB_ADDR_WIDTH-1:WB_ADDR_WIDTH-$bits(WB_RAM_BASE)] == WB_RAM_BASE;

    // Timing for AS6C1008-55PCN
    // (See: https://www.alliancememory.com/wp-content/uploads/pdf/AS6C1008feb2007.pdf)
    //
    //              |<-- 55ns -->|     |<-- 20ns ->|
    //              |            |     |           |
    // ADDR  -------<_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​​​̅_​​>----------------
    //                           |     |           |
    //   OE  _______/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\________________
    //                           |     |           |
    // DOUT  -------------------<​̅_​̅_​̅_​̅_​​̅_​​​̅_​X_​̅_​̅_​̅_​​​​̅_​X_​̅_​̅_​̅_​​̅_​​​>----

    // DOUT valid 55ns after coincident ADDR and OE: max(tAA, tOE, tOLZ)
    //
    //   Address Access Time (tAA)               : 55 ns
    //   Output Enable Access Time (tOE)         : 30 ns
    //   Output Enable to Output in Low-Z (tOLZ) :  5 ns

    // DOUT held 10ns after ADDR changes.
    // DOUT returns to High-Z 20ns after OE deasserted.
    //
    //   Output Hold from Address Change (tOH)   : 10 ns
    //   

    // Write cycle when Address, WE, and DIN are coincident:
    //
    //   Requires 45ns pulse width.
    //   ADDR setup time and DIN hold times are both 0.
    //
    //              |<-- 45ns -->|
    //              |            |
    // ADDR  -------<_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​>-----
    //                           |
    //   WE  _______/‾‾‾‾‾‾‾‾‾‾‾‾\_____
    //                           |
    //  DIN  ----------------<​̅_​̅_​̅_​​>-----

    localparam write_hold_count = common_pkg::ns_to_cycles(45);

    localparam bit [3:0] READY   = 4'b0001,
                         READING = 4'b0010,
                         HIGHZ   = 4'b0100,
                         WRITING = 4'b1000;

    logic [3:0] state = READY;
    logic [2:0] cycle_count = '0;

    assign ram_data_oe = ram_we_o;

    initial begin
        wb_ack_o   = 0;
        wb_stall_o = 0;
        ram_oe_o   = 0;
        ram_we_o   = 0;
    end

    always_ff @(posedge wb_clock_i) begin
        case (state)
            READY: begin
                wb_ack_o   <= 0;
                wb_stall_o <= 0;
                ram_oe_o   <= 0;
                ram_we_o   <= 0;

                if (wb_select && wb_cycle_i && wb_strobe_i) begin
                    cycle_count <= '0;
                    ram_addr_o  <= wb_addr_i[RAM_ADDR_WIDTH-1:0];
                    ram_data_o  <= wb_data_i;
                    wb_ack_o    <= 0;
                    wb_stall_o  <= 1;
                    ram_oe_o    <= !wb_we_i;
                    ram_we_o    <=  wb_we_i;
                    state <= wb_we_i
                        ? WRITING
                        : READING;
                end
            end
            READING: begin
                if (cycle_count == 3) begin
                    cycle_count <= '0;
                    wb_data_o <= ram_data_i;
                    wb_ack_o  <= 1;
                    ram_oe_o  <= 0;
                    state <= HIGHZ;
                end else cycle_count <= cycle_count + 1'b1;
            end
            HIGHZ: begin
                wb_ack_o <= 0;
                wb_stall_o <= 0;
                state <= READY;
            end
            WRITING: begin
                if (cycle_count == write_hold_count) begin
                    wb_ack_o <= 1;
                    wb_stall_o <= 0;
                    ram_we_o  <= 0;
                    state <= READY;
                end
                else cycle_count <= cycle_count + 1'b1;
            end
        endcase
    end
endmodule
