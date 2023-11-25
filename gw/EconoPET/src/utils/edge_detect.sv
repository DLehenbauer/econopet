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

module edge_detect(
    input  logic clk_i,     // Sampling clock
    input  logic data_i,    // Input signal
    output logic pe_o,      // Pulse for rising edge
    output logic ne_o       // Pulse for falling edge
);
    logic q = '0;

    assign pe_o =  data_i && !q;
    assign ne_o = !data_i &&  q;

    always @(posedge clk_i) begin
        q <= data_i;
    end
endmodule
