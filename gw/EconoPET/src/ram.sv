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
    input  logic                      wb_clock_i,
    input  logic                      wb_reset_i,
    input  logic [RAM_ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [    DATA_WIDTH-1:0] wb_data_i,    // Incoming data to write to RAM
    output logic [    DATA_WIDTH-1:0] wb_data_o,    // Outgoing data read from RAM
    input  logic                      wb_we_i,
    input  logic                      wb_cycle_i,
    input  logic                      wb_strobe_i,
    output logic                      wb_stall_o,
    output logic                      wb_ack_o,

    output logic                      ram_oe_o,
    output logic                      ram_we_o,
    output logic [RAM_ADDR_WIDTH-1:0] ram_addr_o,
    input  logic [    DATA_WIDTH-1:0] ram_data_i,
    output logic [    DATA_WIDTH-1:0] ram_data_o,
    output logic                      ram_data_oe
);
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
    localparam read_setup_count = common_pkg::ns_to_cycles(55);

    // DOUT held 10ns after ADDR changes.
    // DOUT returns to High-Z 20ns after OE deasserted.
    //
    //   Output Hold from Address Change (tOH)   : 10 ns
    //   
    localparam read_hold_count = common_pkg::ns_to_cycles(20);

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

    //                      AWOS
    localparam READY   = 4'b0000,
               READING = 4'b0011,
               HIGHZ   = 4'b0001,
               WRITING = 4'b0101,
               DONE    = 4'b1000;

    logic [3:0] state = READY;
    logic [2:0] clk_counter = '0;

    assign wb_stall_o  = state[0];
    assign ram_oe_o    = state[1];
    assign ram_we_o    = state[2];
    assign wb_ack_o    = state[3];
    assign ram_data_oe = ram_we_o;

    always_ff @(posedge wb_clock_i) begin
        if (wb_reset_i) begin
            state <= READY;
            clk_counter <= '0;
        end else begin
            case (state)
                READY: begin
                    if (wb_cycle_i && wb_strobe_i) begin
                        clk_counter <= '0;
                        ram_addr_o <= wb_addr_i;
                        ram_data_o <= wb_data_i;
                        state <= wb_we_i
                            ? WRITING
                            : READING;
                    end
                end
                READING: begin
                    if (clk_counter == read_setup_count) begin
                        clk_counter <= '0;
                        wb_data_o <= ram_data_i;
                        state <= HIGHZ;
                    end else clk_counter <= clk_counter + 1'b1;
                end
                HIGHZ: begin
                    if (clk_counter == read_hold_count) state <= DONE;
                    else clk_counter <= clk_counter + 1'b1;
                end
                WRITING: begin
                    if (clk_counter == write_hold_count) state <= DONE;
                    else clk_counter <= clk_counter + 1'b1;
                end
                DONE : begin
                    state <= READY;
                end
            endcase
        end
    end
endmodule