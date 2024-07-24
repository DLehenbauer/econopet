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

module bram #(
    parameter DATA_DEPTH = 1024,
    parameter ADDR_WIDTH = $clog2(DATA_DEPTH - 1),
    parameter DATA_WIDTH = 8
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
    output logic                  wb_ack_o
);
    logic [DATA_WIDTH-1:0] mem[(2 ** ADDR_WIDTH)-1:0];

    initial begin
        wb_ack_o   = '0;
        wb_stall_o = '0;
    end

    always_ff @(posedge wb_clock_i) begin
        if (!wb_reset_i && wb_cycle_i && wb_strobe_i) begin
            wb_data_o <= mem[wb_addr_i];
            if (wb_we_i) begin
                mem[wb_addr_i] <= wb_data_i;
            end
            wb_ack_o <= 1'b1;
        end else begin
            wb_ack_o <= '0;
        end
    end
endmodule
