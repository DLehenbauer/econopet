
// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

module sync2_edge_detect #(
    INITAL_DATA_I = '0
) (
    input  logic clock_i,  // Destination/sampling clock
    input  logic data_i,   // Input data in source clock domain
    output logic data_o,   // Synchronized output in destination clock domain
    output logic pe_o,     // Pulse for rising edge in destination clock domain
    output logic ne_o      // Pulse for falling edge in destination clock domain
);
    sync2 #(
        INITAL_DATA_I
    ) sync_valid (
        .clock_i(clock_i),
        .data_i (data_i),
        .data_o (data_o)
    );

    edge_detect #(
        INITAL_DATA_I
    ) sync2_edge_detect (
        .clock_i(clock_i),
        .data_i(data_o),
        .pe_o(pe_o),
        .ne_o(ne_o)
    );
endmodule
