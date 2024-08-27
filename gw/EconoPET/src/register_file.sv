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

module register_file(
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

    output logic                     cpu_ready_o,
    output logic                     cpu_reset_o
);
    (* syn_ramstyle = "registers" *) reg [DATA_WIDTH-1:0] register[REG_COUNT:0];

    initial begin
        wb_ack_o   = '0;
        wb_stall_o = '0;

        register[REG_CPU][REG_CPU_READY_BIT] = 1'b0;
        register[REG_CPU][REG_CPU_RESET_BIT] = 1'b1;
    end

    logic wb_select;
    wb_decode #(WB_REG_BASE) wb_decode (
        .wb_addr_i(wb_addr_i),
        .selected_o(wb_select)
    );

    wire [REG_ADDR_WIDTH-1:0] reg_addr = wb_addr_i[REG_ADDR_WIDTH-1:0];

    always_ff @(posedge wb_clock_i) begin
        if (wb_select && wb_cycle_i && wb_strobe_i) begin
            wb_data_o <= register[reg_addr];
            if (wb_we_i) begin
                register[reg_addr] <= wb_data_i;
            end
            wb_ack_o <= 1'b1;
        end else begin
            wb_ack_o <= '0;
        end
    end

    assign cpu_ready_o = register[REG_CPU][REG_CPU_READY_BIT];
    assign cpu_reset_o = register[REG_CPU][REG_CPU_RESET_BIT];
endmodule
