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
    output logic                     wb_data_oe,
    input  logic                     wb_we_i,
    input  logic                     wb_cycle_i,
    input  logic                     wb_strobe_i,
    output logic                     wb_stall_o,
    output logic                     wb_ack_o,

    // CPU register
    output logic                     cpu_ready_o,
    output logic                     cpu_reset_o,

    // Video register
    input  logic                     video_graphic_i,       // VIA CA2 (0 = graphics, 1 = text)
    output logic                     video_col_80_mode_o,
    output logic [11:10]             video_ram_mask_o
);
    logic [DATA_WIDTH-1:0] register[REG_COUNT-1:0];

    initial begin
        wb_data_oe = '0;
        wb_ack_o   = '0;
        wb_stall_o = '0;

        // CPU state at power on: CPU is not ready, CPU is reset
        register[REG_CPU][REG_CPU_READY_BIT] = 1'b0;
        register[REG_CPU][REG_CPU_RESET_BIT] = 1'b1;

        // Video state at power on: 40 column mode, graphics
        register[REG_VIDEO][REG_VIDEO_COL_80_BIT] = 1'b0;
        register[REG_VIDEO][REG_VIDEO_GRAPHICS_BIT] = 1'b0;

        video_ram_mask_o[11:10] = 2'b00;
    end

    logic wb_select;
    wb_decode #(WB_REG_BASE) wb_decode (
        .wb_addr_i(wb_addr_i),
        .selected_o(wb_select)
    );

    wire [REG_ADDR_WIDTH-1:0] reg_addr = wb_addr_i[REG_ADDR_WIDTH-1:0];

    always_ff @(posedge wb_clock_i) begin
        if (wb_select && wb_cycle_i && wb_strobe_i) begin
            wb_data_o  <= register[reg_addr];
            wb_data_oe <= !wb_we_i;
            if (wb_we_i) begin
                register[reg_addr] <= wb_data_i;
            end
            wb_ack_o   <= 1'b1;
        end else begin
            wb_data_oe <= '0;
            wb_ack_o   <= '0;

            // Refresh status registers while not in a wishbone cycle.  This happens at
            // 64 MHz, which is guaranteed to restore overwritten status bits before
            // we process the next SPI command.
            register[REG_VIDEO][REG_VIDEO_GRAPHICS_BIT] <= video_graphic_i;
        end
    end

    assign cpu_ready_o         = register[REG_CPU][REG_CPU_READY_BIT];
    assign cpu_reset_o         = register[REG_CPU][REG_CPU_RESET_BIT];
    assign video_col_80_mode_o = register[REG_VIDEO][REG_VIDEO_COL_80_BIT];
endmodule
