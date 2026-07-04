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

    initial clock_o = '0;

    // Use a level-sensitive 'wait(enable)' rather than 'always @(posedge enable)':
    // the latter can miss the 0->1 transition that 'start' drives at time 0 (a
    // delta race that Icarus tolerates but Verilator does not).
    always begin
        wait (enable);
        #(PERIOD / 4.0);
        clock_o <= 1'b1;
        #(PERIOD / 2.0);
        clock_o <= '0;
        #(PERIOD / 4.0);
    end

    task start;
        clock_o = '0;
        enable  = 1'b1;
    endtask

    task stop;
        enable = '0;
    endtask
endmodule
