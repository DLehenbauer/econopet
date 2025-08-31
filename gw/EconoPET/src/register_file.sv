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
    input  logic                     wb_sel_i,              // Asserted when selected by 'wb_addr_i'

    // Status register
    input  logic                     video_graphic_i,       // VIA CA2 (0 = graphics, 1 = text)
    input  logic                     config_crt_i,          // Display type (0 = 12"/CRTC/20kHz, 1 = 9"/non-CRTC/15kHz)
    input  logic                     config_keyboard_i,     // Keyboard type (0 = Business, 1 = Graphics)

    // CPU register
    output logic                     cpu_ready_o,
    output logic                     cpu_reset_o,
    output logic                     cpu_nmi_o,

    // Video register
    output logic                     video_col_80_mode_o,
    output logic [11:10]             video_ram_mask_o
);
    logic [DATA_WIDTH-1:0] register[REG_COUNT-1:0];

    initial begin
        wb_ack_o   = '0;

        register[REG_STATUS][REG_STATUS_GRAPHICS_BIT] = 1'b0;
        register[REG_STATUS][REG_STATUS_CRT_BIT]      = 1'b0;
        register[REG_STATUS][REG_STATUS_KEYBOARD_BIT] = 1'b0;

        // CPU state at power on:
        register[REG_CPU][REG_CPU_READY_BIT] = 1'b0;    // Not ready
        register[REG_CPU][REG_CPU_RESET_BIT] = 1'b1;    // Reset
        register[REG_CPU][REG_CPU_NMI_BIT]   = 1'b0;    // Not NMI

        // Video state at power on: 40 column mode, graphics
        register[REG_VIDEO][REG_VIDEO_COL_80_BIT] = 1'b0;

        video_ram_mask_o[11:10] = 2'b00;
    end

    // This peripheral always completes WB operations in a single cycle.
    assign wb_stall_o = 1'b0;

    wire [REG_ADDR_WIDTH-1:0] reg_addr = wb_addr_i[REG_ADDR_WIDTH-1:0];

    always_ff @(posedge wb_clock_i) begin
        if (wb_sel_i && wb_cycle_i && wb_strobe_i) begin
            wb_data_o <= register[reg_addr];
            if (wb_we_i) begin
                register[reg_addr] <= wb_data_i;
            end
            wb_ack_o <= 1'b1;
        end else begin
            wb_ack_o <= '0;

            // Refresh status registers while not in a wishbone cycle.  This happens at
            // 64 MHz, which is guaranteed to restore overwritten status bits before
            // we process the next SPI command.
            //
            // Order must match bit order declared in common_pkg.svh.
            register[REG_STATUS] <= { 5'b0000_0, config_keyboard_i, config_crt_i, video_graphic_i};
        end
    end

    assign cpu_ready_o         = register[REG_CPU][REG_CPU_READY_BIT];
    assign cpu_reset_o         = register[REG_CPU][REG_CPU_RESET_BIT];
    assign cpu_nmi_o           = register[REG_CPU][REG_CPU_NMI_BIT];
    
    assign video_col_80_mode_o = register[REG_VIDEO][REG_VIDEO_COL_80_BIT];
endmodule
