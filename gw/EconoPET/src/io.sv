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

// This module tracks and intercepts select reads/writes to I/O registers.
// This is used to inject USB keyboard input.
module io(
    input  logic                     wb_clock_i,

    input  logic                     cpu_be_i,
    input  logic [   DATA_WIDTH-1:0] cpu_data_i,
    output logic [   DATA_WIDTH-1:0] cpu_data_o,
    output logic                     cpu_data_oe,   // Asserted when intercepting CPU read
    input  logic                     cpu_we_i,

    input  logic                     pia1_cs_i,     // PIA1 chip select (from address_decoding)
    input  logic                     pia2_cs_i,     // PIA2 chip select (from address_decoding)
    input  logic                     via_cs_i,      // VIA chip select (from address_decoding)
    input  logic [ VIA_RS_WIDTH-1:0] rs_i,          // Register select (cpu_addr[3:0])

    input  logic [KBD_COL_COUNT-1:0][KBD_ROW_COUNT-1:0] usb_kbd_i  // State of USB keyboard matrix
);
    // CPU writes to I/O registers are cached here.
    logic [DATA_WIDTH-1:0] register[IO_REG_COUNT-1:0];
    
    wire [PIA_RS_WIDTH-1:0] pia_rs = rs_i[PIA_RS_WIDTH-1:0];                // PIA register select is A[1:0]
    wire [VIA_RS_WIDTH-1:0] via_rs = rs_i[VIA_RS_WIDTH-1:0];                // VIA register select is A[3:0]
    
    logic [PIA1_PORTA_KEY_D_OUT:PIA1_PORTA_KEY_A_OUT] selected_col      = '0;
    logic [KBD_ROW_COUNT-1:0]                         selected_row_data = '1;

    always_ff @(posedge wb_clock_i) begin
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
                unique case (1'b1)
                    pia1_cs_i: begin
                        unique case (pia_rs)
                            // Inject USB keyboard input by intercepting reads from PIA1_PORTB when
                            // a USB keyboard key is pressed.
                            PIA_PORTB: begin
                                cpu_data_o  <= selected_row_data;             // Load USB keyboard column into 'cpu_data_o' register.
                                cpu_data_oe <= selected_row_data != 8'hff;    // But only assert 'cpu_data_oe' if a key is pressed.
                            end
                            default: /* do nothing */;
                        endcase
                    end
                    default: /* do nothing */ ;
                endcase
            end
        end else begin
            // To relax timing, we pipeline reads from the 'register' array.
            selected_row_data   <= usb_kbd_i[selected_col];
            selected_col        <= register[REG_IO_PIA1_PORTA][PIA1_PORTA_KEY_D_OUT:PIA1_PORTA_KEY_A_OUT];
        end
    end
endmodule
