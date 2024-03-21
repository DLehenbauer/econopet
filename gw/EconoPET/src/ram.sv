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

module ram #(
    parameter WB_CLOCK_MHZ = 64,
    parameter DATA_WIDTH   = 8,
    parameter ADDR_WIDTH   = 17
) (
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                  wb_clock_i,
    input  logic                  wb_reset_i,
    input  logic [ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [DATA_WIDTH-1:0] wb_data_i,
    output logic [DATA_WIDTH-1:0] wb_data_o,
    input  logic                  wb_we_i,
    input  logic                  wb_cycle_i,
    input  logic                  wb_strobe_i,
    output logic                  wb_stall_o,
    output logic                  wb_ack_o,

    output logic                  ram_oe_o,
    output logic                  ram_we_o,
    output logic [ADDR_WIDTH-1:0] ram_addr_o,
    input  logic [DATA_WIDTH-1:0] ram_data_i,
    output logic [DATA_WIDTH-1:0] ram_data_o,
    output logic                  ram_data_oe
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

    //                      S
    localparam READY   = 1'b0,
               READING = 1'b1;

    logic [0:0] state = READY;

    assign wb_stall_o = state[0];

    initial begin
        ram_oe_o    = '0;
        ram_we_o    = '0;
        ram_data_oe = '0;
        wb_ack_o    = '0;
    end

    always_ff @(posedge wb_clock_i) begin
        if (wb_reset_i) begin
            state       <= READY;
            ram_oe_o    <= '0;
            ram_we_o    <= '0;
            ram_data_oe <= '0;
            wb_ack_o    <= '0;
        end else begin
            case (state)
                READY: begin
                    wb_data_o <= ram_data_i;
                    wb_ack_o  <= ram_oe_o | ram_we_o;

                    ram_oe_o    <= '0;
                    ram_we_o    <= '0;
                    ram_data_oe <= '0;

                    if (wb_cycle_i && wb_strobe_i) begin
                        ram_addr_o  <= wb_addr_i;
                        ram_data_o  <= wb_data_i;
                        ram_oe_o    <= !wb_we_i;
                        ram_we_o    <= wb_we_i;
                        ram_data_oe <= wb_we_i;

                        state <= wb_we_i
                            ? READY
                            : READING;
                    end
                end

                READING: begin
                    state <= READY;
                end
            endcase
        end
    end
endmodule
