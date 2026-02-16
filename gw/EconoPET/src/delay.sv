// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

module delay #(
    parameter int DELAY_CYCLES = 2,           // Number of clock cycles to delay the signal
    parameter bit INITIAL_VALUE = 1'b0        // Initial state of the delay line
) (
    input  logic clock_i,                     // Clock signal
    input  logic reset_i,                     // Reset signal (active high)
    input  logic data_i,                      // Input data to be delayed
    output logic data_o                       // Delayed output data
);
    // Create a delay shift register with configurable length
    logic [DELAY_CYCLES-1:0] delay_line;

    initial begin
        delay_line = {DELAY_CYCLES{INITIAL_VALUE}};
    end

    always_ff @(posedge clock_i) begin
        if (reset_i) begin
            delay_line <= {DELAY_CYCLES{INITIAL_VALUE}};
        end else begin
            delay_line <= {delay_line[DELAY_CYCLES-2:0], data_i};
        end
    end

    // Output is the most delayed bit
    assign data_o = delay_line[DELAY_CYCLES-1];
endmodule
