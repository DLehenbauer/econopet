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

module timing (
    input  logic clock_i,
    output logic clk1_en_o,
    output logic clk8_en_o,
    output logic clk16_en_o
);
    initial begin
        clk1_en_o  = '0;
        clk8_en_o  = '0;
        clk16_en_o = '0;
    end

    logic [5:0] cycle_count = '0;

    always_ff @(posedge clock_i) begin
        cycle_count <= cycle_count + 1'b1;
    end

    always_ff @(posedge clock_i) begin
        clk1_en_o  <= cycle_count[5:0] == 6'b000000;
        clk8_en_o  <= cycle_count[2:0] == 3'b000;
        clk16_en_o <= cycle_count[1:0] == 2'b00;
    end
endmodule
