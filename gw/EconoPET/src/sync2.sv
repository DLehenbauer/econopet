// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

module sync2 #(
    INITAL_DATA_I = '0,
    INITAL_DATA_O = INITAL_DATA_I
) (
    input  logic clock_i,  // Destination clock
    input  logic data_i,   // Input data in source clock domain
    output logic data_o    // Synchronized output in destination clock domain
);
    initial begin
        q[1:0] = { INITAL_DATA_O, INITAL_DATA_I };
    end

    (* async_reg = "true" *) reg [1:0] q;

    always_ff @(posedge clock_i) begin
        {q[1], q[0]} <= {q[0], data_i};
    end

    assign data_o = q[1];
endmodule
