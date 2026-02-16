// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

module clock_gen #(
    parameter MHZ = 24    // Clock speed in MHz
) (
    output logic clock_o  // Destination clock
);
    localparam real PERIOD = common_pkg::mhz_to_ns(MHZ);

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
