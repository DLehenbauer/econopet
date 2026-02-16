// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

module edge_detect #(
    INITAL_DATA_I = '0
) (
    input  logic clock_i,  // Sampling clock
    input  logic data_i,   // Input signal
    output logic pe_o,     // Pulse for rising edge
    output logic ne_o      // Pulse for falling edge
);
    logic q = INITAL_DATA_I;

    assign pe_o =  data_i && !q;
    assign ne_o = !data_i &&  q;

    always @(posedge clock_i) begin
        q <= data_i;
    end
endmodule
