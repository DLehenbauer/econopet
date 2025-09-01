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
    input  logic [WB_ADDR_WIDTH-1:0] wbp_addr_i,
    input  logic [   DATA_WIDTH-1:0] wbp_data_i,
    output logic [   DATA_WIDTH-1:0] wbp_data_o,
    input  logic                     wbp_we_i,
    input  logic                     wbp_cycle_i,
    input  logic                     wbp_strobe_i,
    output logic                     wbp_stall_o,
    output logic                     wbp_ack_o,
    input  logic                     wbp_sel_i,              // Asserted when selected by 'wbp_addr_i'

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
        wbp_ack_o   = '0;

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
    assign wbp_stall_o = 1'b0;

    wire [REG_ADDR_WIDTH-1:0] reg_addr = wbp_addr_i[REG_ADDR_WIDTH-1:0];

    always_ff @(posedge wb_clock_i) begin
        if (wbp_sel_i && wbp_cycle_i && wbp_strobe_i) begin
            wbp_data_o <= register[reg_addr];
            if (wbp_we_i) begin
                register[reg_addr] <= wbp_data_i;
            end
            wbp_ack_o <= 1'b1;
        end else begin
            wbp_ack_o <= '0;

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
