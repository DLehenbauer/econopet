// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// Wishbone peripheral that:
//
//  1. Ingests USB keyboard matrix data from the MCU and injects it when the
//     CPU reads PIA1 keyboard input.
//
//  2. Observes CPU and MCU bridge PIA1 scans so the last-read matrix can
//     later be fetched over Wishbone. Bridge reads remain raw physical input,
//     while CPU observations retain any USB injection.
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
    input  logic                     cpu_we_i,          // CPU write enable
    output logic [   DATA_WIDTH-1:0] cpu_data_o,        // Outgoing data to system bus when intercepting reads
    output logic                     cpu_data_oe,       // Output enable when when intercepting reads

    input  logic                     pia1_cs_i,         // PIA1 chip select (from address decoding)
    input  logic [ PIA_RS_WIDTH-1:0] pia1_rs_i,          // PIA1 register select (cpu_addr[1:0])

    input  logic                     pia1_access_i,     // Captured, executed PIA1 access from either owner
    input  logic [ PIA_RS_WIDTH-1:0] pia1_access_rs_i,
    input  logic                     pia1_access_we_i,
    input  logic [   DATA_WIDTH-1:0] pia1_access_data_i
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

    localparam int PIA_DATA_SELECT_BIT = 2;
    logic port_a_selected = 1'b0;
    logic port_b_selected = 1'b0;
    logic [KBD_COL_WIDTH-1:0] row_latch = '0;
    logic [KBD_COL_WIDTH-1:0] row_direction = '0;
    // Undriven row-select pins are pulled high on the physical keyboard.
    wire [KBD_COL_WIDTH-1:0] selected_col = row_latch | ~row_direction;
    wire valid_col = selected_col < KBD_COL_COUNT;
    logic [KBD_ROW_COUNT-1:0] selected_row_data = '1;

    always_ff @(posedge wb_clock_i) begin
        selected_row_data <= valid_col ? usb_kbd[selected_col] : '1;

        // Both owners modify the same PIA. DDR accesses must not look like scans.
        if (pia1_access_i) begin
            if (pia1_access_we_i) begin
                case (pia1_access_rs_i)
                    PIA_CRA: port_a_selected <= pia1_access_data_i[PIA_DATA_SELECT_BIT];
                    PIA_CRB: port_b_selected <= pia1_access_data_i[PIA_DATA_SELECT_BIT];
                    PIA_PORTA: begin
                        if (port_a_selected)
                            row_latch <= pia1_access_data_i[KBD_COL_WIDTH-1:0];
                        else
                            row_direction <= pia1_access_data_i[KBD_COL_WIDTH-1:0];
                    end
                    default: /* do nothing */;
                endcase
            end else if (pia1_access_rs_i == PIA_PORTB && port_b_selected && valid_col) begin
                pet_kbd[selected_col] <= pia1_access_data_i;
            end
        end
    end

    always_ff @(posedge wb_clock_i) begin
        cpu_data_oe <= 1'b0;

        if (cpu_be_i && !cpu_we_i && port_b_selected && valid_col) begin
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
