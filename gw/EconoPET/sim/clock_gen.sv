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

module clock_gen #(
    parameter MHZ = 24    // Clock speed in MHz
) (
    output logic clock_o  // Destination clock
);
    localparam PERIOD = common_pkg::mhz_to_ns(MHZ);

    bit enable = '0;

    always @(posedge enable) begin
        while (enable) begin
            #(PERIOD / 4.0);
            clock_o <= 1'b1;
            #(PERIOD / 2.0);
            clock_o <= '0;
            #(PERIOD / 4.0);
        end
    end

    task start;
        clock_o = '0;
        enable  = 1'b1;
    endtask

    task stop;
        enable = '0;
    endtask
endmodule
