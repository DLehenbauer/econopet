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
    output logic cpu_grant_o,
    output logic wb_grant_o
);
    logic [5:0] cycle_count = '0;

    always_ff @(posedge clock_i) begin
        cycle_count <= cycle_count + 1'b1;
    end

    logic cpu_grant;
    logic wb_grant;

    always_comb begin
        cpu_grant_o = cycle_count == 6'd0;

        wb_grant_o  = cycle_count == 6'd16
            || cycle_count == 6'd24
            || cycle_count == 6'd32
            || cycle_count == 6'd40
            || cycle_count == 6'd48
            || cycle_count == 6'd56;
    end
endmodule
