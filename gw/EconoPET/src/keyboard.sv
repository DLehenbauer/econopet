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

    input  logic                     cpu_be_i,
    input  logic [   DATA_WIDTH-1:0] cpu_data_i,
    output logic [   DATA_WIDTH-1:0] cpu_data_o,
    output logic                     cpu_data_oe,   // Asserted when intercepting CPU read
    input  logic                     cpu_we_i,

    input  logic                     pia1_cs_i,     // PIA1 chip select (from address_decoding)
    input  logic                     pia2_cs_i,     // PIA2 chip select (from address_decoding)
    input  logic                     via_cs_i,      // VIA chip select (from address_decoding)
    input  logic [ VIA_RS_WIDTH-1:0] rs_i           // Register select (cpu_addr[3:0])
);
    logic [DATA_WIDTH-1:0] register[IO_REG_COUNT-1:0];

    initial begin
        logic [IO_REG_ADDR_WIDTH-1:0] col;

        // PET keyboard matrix is 10 columns x 8 rows.  When a key is pressed, the corresponding bit
        // in the matrix is cleared to 0.  When no key is pressed, the bit is set to 1.
        for (col = REG_IO_KEY_MATRIX_START; col <= REG_IO_KEY_MATRIX_END; col = col + 1) begin
            register[col] = 8'hFF;  // no keys pressed
        end
    end
    
    wire [PIA_RS_WIDTH-1:0] pia_rs = rs_i[PIA_RS_WIDTH-1:0];                // PIA register select is A[1:0]
    wire [VIA_RS_WIDTH-1:0] via_rs = rs_i[VIA_RS_WIDTH-1:0];                // VIA register select is A[3:0]
    wire [IO_REG_ADDR_WIDTH-1:0] wb_rs = wb_addr_i[IO_REG_ADDR_WIDTH-1:0];  // WB register select
    
    logic [PIA1_PORTA_KEY_D_OUT:PIA1_PORTA_KEY_A_OUT] selected_col = '0;
    logic [DATA_WIDTH-1:0]                            current_col  = 8'hff;

    // This peripheral always completes WB operations in a single cycle.
    assign wb_stall_o = 1'b0;

    always_ff @(posedge wb_clock_i) begin
        wb_ack_o <= '0;
        cpu_data_oe <= '0;

        if (cpu_be_i) begin
            if (cpu_we_i) begin
                // Cache CPU writes to all I/O registers.
                unique case (1'b1)
                    pia1_cs_i: register[{ REG_IO_PIA1, pia_rs }] <= cpu_data_i;
                    pia2_cs_i: register[{ REG_IO_PIA2, pia_rs }] <= cpu_data_i;
                    via_cs_i:  register[{ REG_IO_VIA,  via_rs }] <= cpu_data_i;
                    default: /* do nothing */ ;
                endcase
            end else begin
                // Intercept CPU reads from select I/O registers.
                case (1'b1)
                    pia1_cs_i: begin
                        unique case (pia_rs)
                            // Inject USB keyboard input by intercepting reads from PIA1_PORTB when
                            // a USB keyboard key is pressed.
                            PIA_PORTB: begin
                                cpu_data_o  <= current_col;             // Load USB keyboard column into 'cpu_data_o' register.
                                cpu_data_oe <= current_col != 8'hff;    // But only assert 'cpu_data_oe' if a key is pressed.
                            end
                            default: /* do nothing */;
                        endcase
                    end
                    default: /* do nothing */ ;
                endcase
            end
        end else if (wb_sel_i && wb_cycle_i && wb_strobe_i) begin
            if (wb_we_i) begin
                register[wb_rs] <= wb_data_i;
            end
            wb_data_o <= register[wb_rs];
            wb_ack_o <= 1'b1;
        end else begin
            // To relax timing, we pipeline reads from the 'register' block ram.
            current_col <= register[{ REG_IO_KBD, selected_col }];
            selected_col <= register[REG_IO_PIA1_PORTA][PIA1_PORTA_KEY_D_OUT:PIA1_PORTA_KEY_A_OUT];
        end
    end
endmodule
