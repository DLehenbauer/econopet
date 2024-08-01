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
    // Timing for IS61WV1288EEBLL-10TLI
    // (See: https://www.issi.com/WW/pdf/61-64WV1288EEBLL.pdf)
    //
    // Read cycle when Address and OE are coincident:
    //
    //   DOUT valid 10ns after coincident ADDR and OE.
    //   Pevious DOUT held 2ns after ADDR changes.
    //   Requires 4ns to return to High-Z after OE deasserted.
    //
    //              |<-- 10ns -->|    |<- 4ns ->|
    //              |            |    |         |
    // ADDR  -------<_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​​>----------------
    //                           |    |         |
    //   OE  _______/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\________________
    //                           |    |         |
    // DOUT  --------------------<_​̅_​̅_​​̅_​X_​̅_​̅_​̅_​​X_​̅_​̅_​̅_​​>------
    //
    // Write cycle when Address, WE, and DIN are coincident:
    //
    //   Requires 10ns pulse width (8ns if OE deasserted)
    //   ADDR setup time and DIN hold times are both 0.
    //
    //              |<-- 10ns -->|
    //              |            |
    // ADDR  -------<_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​̅_​>-----
    //                           |
    //   WE  _______/‾‾‾‾‾‾‾‾‾‾‾‾\_____
    //                           |
    //  DIN  ----------------<_​̅_​̅_​​>-----

    localparam RAM_tAA       = 55,  // RAM Address Access Time
               RAM_tOE       = 30,  // Output Enable Access Time
               RAM_tOLZ      = 5,   // Output Enable to Output in Low-Z
               RAM_tOHZ      = 20,  // Output Disable to Output in High-Z
               RAM_tOH       = 10;  // Output Hold from Address Change

    localparam RAM_tAW       = 50,  // Address Valid to End of Write
               RAM_tAS       = 0,   // Address Setup Time
               RAM_tWP       = 45,  // Write Pulse Width
               RAM_tWR       = 0,   // Write Recovery Time
               RAM_tDW       = 25,  // Data to Write Time Overlap
               RAM_tDH       = 0,   // Data Hold from End of Write Time
               RAM_tOW       = 5,   // Output Active from End of Write
               RAM_tWHZ      = 20;  // Write to Output in High-Z


    localparam bit [2:0] READ_DONE  = common_pkg::ns_to_cycles(RAM_tAA),
                         WRITE_DONE = common_pkg::ns_to_cycles(RAM_tAA);

    localparam bit [1:0] READY   = 2'd0,
                         READ    = 2'd1,
                         WRITE   = 2'd2,
                         DONE    = 2'd3;

    logic [2:0] count;
    logic [1:0] state;

    initial begin
        state       = READY;
        count       = '0;
        ram_oe_o    = '0;
        ram_we_o    = '0;
        ram_data_oe = '0;
        wb_ack_o    = '0;
        wb_stall_o  = '0;
    end

    always_ff @(posedge wb_clock_i) begin
        if (wb_reset_i) begin
            state       <= READY;
            ram_oe_o    <= '0;
            ram_we_o    <= '0;
            ram_data_oe <= '0;
            wb_ack_o    <= '0;
            wb_stall_o  <= '0;
            count       <= '0;
        end else begin
            case (state)
                READY: begin
                    wb_ack_o    <= '0;
                    wb_stall_o  <= '0;
                    ram_oe_o    <= '0;
                    ram_we_o    <= '0;
                    ram_data_oe <= '0;
                                            
                    count <= wb_we_i
                        ? WRITE_DONE - 2
                        : READ_DONE - 2;

                    if (wb_cycle_i && wb_strobe_i) begin
                        ram_addr_o  <= wb_addr_i;
                        ram_data_o  <= wb_data_i;
                        ram_data_oe <= wb_we_i;
                        ram_oe_o    <= !wb_we_i;
                        wb_stall_o  <= 1'b1;

                        state <= wb_we_i
                            ? WRITE
                            : READ;
                    end
                end

                READ: begin
                    if (count == 0) begin
                        state       <= DONE;
                    end else begin
                        state       <= READ;
                    end

                    count <= count - 1'b1;
                end

                WRITE: begin
                    ram_we_o    <= 1'b1;

                    if (count == 0) begin
                        state       <= DONE;
                    end else begin
                        state       <= WRITE;
                    end

                    count <= count - 1'b1;
                end

                DONE: begin
                    wb_data_o   <= ram_data_i;
                    ram_oe_o    <= '0;
                    ram_we_o    <= '0;
                    ram_data_oe <= '0;
                    wb_stall_o  <= '0;
                    wb_ack_o    <= '1;
                    state       <= READY;
                end
            endcase
        end
    end
endmodule
