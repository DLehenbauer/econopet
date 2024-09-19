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
    input  logic        pixel_clk_en,           // Pixel clock enable
    input  logic        video_latch,
    input  logic [15:0] pixels_i,
    input  logic [1:0]  reverse_i,
    input  logic        display_en_i,
    output logic        video_o
);
    logic [3:0]  pixel_ctr_d, pixel_ctr_q;
    logic [15:0] sr_out_d, sr_out_q;
    logic [1:0]  reverse_d, reverse_q;
    logic        display_en_d, display_en_q;

    always_comb begin
        if (video_latch) begin
            pixel_ctr_d = '0;
            sr_out_d = pixels_i;
            reverse_d = reverse_i;
            display_en_d = display_en_i;
        end else begin
            pixel_ctr_d = pixel_ctr_q + 1'b1;
            sr_out_d = { sr_out_q[14:0], 1'b0 };
            reverse_d = reverse_q;
            display_en_d = display_en_q;    // TODO: Was 'display_en_i'?
        end
    end

    always_ff @(posedge sys_clock_i) begin
        if (pixel_clk_en) begin
            pixel_ctr_q  <= pixel_ctr_d;
            sr_out_q     <= sr_out_d;
            reverse_q    <= reverse_d;
            display_en_q <= display_en_d;
        end
    end

    // 'pixels_i' contains pixels for two characters.  The high bit of 'pixel_ctr_q' selects
    // which 'reverse_i' bit to use.
    //
    // TODO: Can avoid the '~' by reversing the order of the bits passed to 'reverse_i' at the caller.
    assign video_o = display_en_q & (sr_out_q[15] ^ reverse_q[~pixel_ctr_q[3]]);
endmodule
