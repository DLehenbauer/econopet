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

module video_dotgen(
    input  logic       sys_clock_i,            // FPGA System clock
    input  logic       pixel_clk_en_i,         // Pixel clock enable
    input  logic       load_sr_i,
    input  logic [7:0] pixels_i,
    input  logic       reverse_i,
    input  logic       display_en_i,
    output logic       video_o
);
    logic [7:0] sr_out;
    logic       display_en;

    logic v_out = '0;
    logic d_out = '0;
    logic r_out = '0;

    always_ff @(posedge sys_clock_i) begin
        if (pixel_clk_en_i) begin
            if (load_sr_i) begin
                v_out <= pixels_i[7];
                d_out <= display_en_i;
                r_out <= reverse_i;

                sr_out     <= pixels_i;
                display_en <= display_en_i;
            end else begin
                v_out      <= sr_out[6];
                sr_out     <= { sr_out[6:0], 1'bx };
            end
        end
    end

    assign video_o = d_out & (v_out ^ r_out);
endmodule
