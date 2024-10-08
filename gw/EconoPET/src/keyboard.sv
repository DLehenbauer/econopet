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

    input  logic [   DATA_WIDTH-1:0] cpu_data_i,
    output logic [   DATA_WIDTH-1:0] cpu_data_o,
    output logic                     cpu_data_oe,           // Asserted when intercepting CPU read of keyboard matrix
    input  logic                     cpu_we_i,

    input  logic                     pia1_cs_i,             // PIA chip select (from address_decoding)
    input  logic [ PIA_RS_WIDTH-1:0] pia1_rs_i              // PIA register select (cpu_addr[1:0])
);
    logic wb_select;
    wb_decode #(WB_KBD_BASE) wb_decode (
        .wb_addr_i(wb_addr_i),
        .selected_o(wb_select)
    );

    // PET keyboard matrix is 10x8.  When a key is pressed, the corresponding bit
    // in the matrix is cleared to 0.  When no key is pressed, the bit is set to 1.
    logic [DATA_WIDTH-1:0] matrix[KBD_ROW_COUNT-1:0];

    initial begin
        integer i;

        for (i = 0; i < KBD_ROW_COUNT; i = i + 1) begin
            matrix[i] = 8'hFF;
        end
    end

    wire [KBD_ADDR_WIDTH-1:0] row_addr = wb_addr_i[KBD_ADDR_WIDTH-1:0];
    wire writing_port_a =  cpu_we_i && pia1_cs_i && pia1_rs_i == PIA_PORTA;     // CPU is selecting next keyboard row
    wire reading_port_b = !cpu_we_i && pia1_cs_i && pia1_rs_i == PIA_PORTB;     // CPU is reading state of currenly selected row

    logic [KBD_ADDR_WIDTH-1:0] selected_row = '0;
    logic [    DATA_WIDTH-1:0] current_row  = 8'hff;

    // This peripheral always completes WB operations in a single cycle.
    assign wb_stall_o = 1'b0;

    always_ff @(posedge wb_clock_i) begin
        wb_ack_o   <= '0;

        if (wb_select && wb_cycle_i && wb_strobe_i) begin
            if (wb_we_i) begin
                matrix[row_addr] <= wb_data_i;
            end else begin
                wb_data_o <= matrix[row_addr];
            end
            wb_ack_o <= 1'b1;
        end else begin
            // To relax timing, we pipeline reads from the 'matrix' block ram.
            cpu_data_o  <= current_row;
            cpu_data_oe <= reading_port_b && current_row != 8'hff;
            current_row <= matrix[selected_row];

            if (writing_port_a) selected_row <= cpu_data_i[KBD_ADDR_WIDTH-1:0];
        end
    end
endmodule
