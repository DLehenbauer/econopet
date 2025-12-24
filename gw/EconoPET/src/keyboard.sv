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

import common_pkg::*;

// Wishbone peripheral that:
//
//  1. Ingests USB keyboard matrix data from the MCU and injects it when the
//     CPU reads PIA1 keyboard input.
//
//  2. Snoops the PET keyboard matrix during CPU scans via PIA1 reads so the
//     observed matrix can later be fetched over Wishbone.
//
// Note that the snooped PET keyboard matrix is only updated when the CPU
// is actively scanning the keyboard via PIA1 reads.
module keyboard(
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_clock_i,        // Wishbone bus clock
    input  logic [WB_ADDR_WIDTH-1:0] wbp_addr_i,        // Wishbone address (low bits pick keyboard column)
    input  logic [   DATA_WIDTH-1:0] wbp_data_i,        // Wishbone write data (USB keyboard column bitmap)
    output logic [   DATA_WIDTH-1:0] wbp_data_o,        // Wishbone read data (PET keyboard column bitmap)
    input  logic                     wbp_we_i,          // Wishbone write enable (1 = write USB column)
    input  logic                     wbp_cycle_i,       // Wishbone cycle in progress
    input  logic                     wbp_strobe_i,      // Wishbone request strobe (addr/data valid)
    output logic                     wbp_stall_o,       // Wishbone stall (not used, always 0)
    output logic                     wbp_ack_o,         // Wishbone acknowledge for completed transfer
    input  logic                     wbp_sel_i,         // Wishbone peripheral select decoded from address

    input  logic                     cpu_be_i,          // CPU bus enable
    input  logic                     cpu_data_strobe_i, // Strobe aligned with CPU data phase
    input  logic                     cpu_we_i,          // CPU write enable
    input  logic [   DATA_WIDTH-1:0] cpu_data_i,        // Incoming data from system bus
    output logic [   DATA_WIDTH-1:0] cpu_data_o,        // Outgoing data to system bus when intercepting reads
    output logic                     cpu_data_oe,       // Output enable when when intercepting reads

    input  logic                     pia1_cs_i,         // PIA1 chip select (from address decoding)
    input  logic [ PIA_RS_WIDTH-1:0] pia1_rs_i          // PIA1 register select (cpu_addr[1:0])
);
    // PET keyboard matrix is all 1's when no keys are pressed.
    logic [KBD_COL_COUNT-1:0][KBD_ROW_COUNT-1:0] usb_kbd = '1;
    logic [KBD_COL_COUNT-1:0][KBD_ROW_COUNT-1:0] pet_kbd = '1;
    
    // Wishbone address bits [3:0] select the column of the keyboard matrix.
    wire [KBD_COL_WIDTH-1:0] wb_rs = wbp_addr_i[KBD_COL_WIDTH-1:0];

    // This peripheral always completes WB operations in a single cycle.
    assign wbp_stall_o = 1'b0;

    always_ff @(posedge wb_clock_i) begin
        wbp_ack_o <= '0;

        if (wbp_sel_i && wbp_cycle_i && wbp_strobe_i) begin
            if (wbp_we_i) begin
                usb_kbd[wb_rs] <= wbp_data_i;
            end
            wbp_data_o <= pet_kbd[wb_rs];
            wbp_ack_o  <= 1'b1;
        end
    end

    // CPU writes to I/O registers are cached here.
    logic [KBD_COL_WIDTH-1:0] selected_col      = '0;
    logic [KBD_ROW_COUNT-1:0] selected_row_data = '1;

    always_ff @(posedge wb_clock_i) begin
        // Cache the selected column so that PIA1_PORTB interception reads can
        // avoid performing a usb_kbd array lookup.
        selected_row_data <= usb_kbd[selected_col];

        if (pia1_cs_i && cpu_data_strobe_i) begin
            if (cpu_we_i) begin
                // CPU writes to PIA1_PORTA select the keyboard column to scan.
                if (pia1_rs_i == PIA_PORTA) begin
                    selected_col <= cpu_data_i[PIA1_PORTA_KEY_D_OUT:PIA1_PORTA_KEY_A_OUT];
                end
            end else begin
                // Snoop CPU reads from PIA1_PORTB to capture PET keyboard state.
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
