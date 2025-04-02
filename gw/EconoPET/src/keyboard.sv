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

    input  logic                     cpu_be_i,
    input  logic                     cpu_data_strobe_i,
    input  logic [   DATA_WIDTH-1:0] cpu_data_i,
    output logic [   DATA_WIDTH-1:0] cpu_data_o,
    output logic                     cpu_data_oe,   // Asserted when intercepting CPU read
    input  logic                     cpu_we_i,

    input  logic                     pia1_cs_i,     // PIA1 chip select (from address_decoding)
    input  logic [ PIA_RS_WIDTH-1:0] pia1_rs_i      // PIA1 register select (cpu_addr[1:0])
);
    // PET keyboard matrix is all 1's when no keys are pressed.
    logic [KBD_COL_COUNT-1:0][KBD_ROW_COUNT-1:0] usb_kbd = '1;
    logic [KBD_COL_COUNT-1:0][KBD_ROW_COUNT-1:0] pet_kbd = '1;
    
    // Wishbone address bits [3:0] select the column of the keyboard matrix.
    wire [KBD_COL_WIDTH-1:0] wb_rs = wb_addr_i[KBD_COL_WIDTH-1:0];

    // This peripheral always completes WB operations in a single cycle.
    assign wb_stall_o = 1'b0;

    always_ff @(posedge wb_clock_i) begin
        wb_ack_o <= '0;

        if (wb_sel_i && wb_cycle_i && wb_strobe_i) begin
            if (wb_we_i) begin
                usb_kbd[wb_rs] <= wb_data_i;
            end
            wb_data_o <= pet_kbd[wb_rs];
            wb_ack_o  <= 1'b1;
        end
    end

    // CPU writes to I/O registers are cached here.
    logic [KBD_COL_WIDTH-1:0] selected_col      = '0;
    logic [KBD_ROW_COUNT-1:0] selected_row_data = '1;

    always_ff @(posedge wb_clock_i) begin
        selected_row_data <= usb_kbd[selected_col];

        if (pia1_cs_i && cpu_data_strobe_i) begin
            if (cpu_we_i) begin
                if (pia1_rs_i == PIA_PORTA) begin
                    selected_col <= cpu_data_i[PIA1_PORTA_KEY_D_OUT:PIA1_PORTA_KEY_A_OUT];
                end
            end else begin
                if (pia1_rs_i == PIA_PORTB) begin
                    pet_kbd[selected_col] <= cpu_data_i;
                end
            end
        end
    end

    always_ff @(posedge wb_clock_i) begin   
        cpu_data_oe <= 1'b0;

        if (cpu_be_i && !cpu_we_i) begin
            // Intercept CPU reads from select I/O registers.
            unique case (1'b1)
                pia1_cs_i: begin
                    unique case (pia1_rs_i)
                        // Inject USB keyboard input by intercepting reads from PIA1_PORTB when
                        // a USB keyboard key is pressed.
                        PIA_PORTB: begin
                            cpu_data_o  <= selected_row_data;             // Load USB keyboard column into 'cpu_data_o' register.
                            cpu_data_oe <= selected_row_data != 8'hff;    // But only assert 'cpu_data_oe' if a key is pressed.
                        end
                        default: /* do nothing */;
                    endcase
                end
                default: /* do nothing */;
            endcase
        end
    end
endmodule
