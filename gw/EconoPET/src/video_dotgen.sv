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

module video_dotgen(
    input  logic        sys_clock_i,            // FPGA System clock
    input  logic        pixel_clk_en_i,         // Pixel clock enable
    input  logic        load_sr_i,
    input  logic [15:0] pixels_i,
    input  logic [1:0]  reverse_i,
    input  logic        display_en_i,
    output logic        video_o
);
    // TODO: The 'pixel_ctr' counter is only used to select the 'reverse' bit.
    //       This can be avoided if we use a 2 MHz CLK in 80 col mode.
    logic [3:0]  pixel_ctr;
    logic [15:0] sr_out;
    logic [1:0]  reverse;
    logic        display_en;

    always_ff @(posedge sys_clock_i) begin
        if (load_sr_i) begin
            pixel_ctr  <= '0;
            sr_out     <= pixels_i;
            reverse    <= reverse_i;
            display_en <= display_en_i;
        end else if (pixel_clk_en_i) begin
            pixel_ctr  <= pixel_ctr + 1'b1;
            sr_out     <= { sr_out[14:0], 1'bx };
            display_en <= display_en_i;             // TODO: Why isn't this captured on cclk?
        end
    end

    // 'pixels_i' contains pixels for two characters.  The high bit of 'pixel_ctr' selects
    // which 'reverse_i' bit to use.
    assign video_o = display_en & (sr_out[15] ^ reverse[pixel_ctr[3]]);
endmodule
