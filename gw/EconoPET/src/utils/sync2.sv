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

module sync2 #(
    INITAL_DATA_I = '0,
    INITAL_DATA_O = INITAL_DATA_I
) (
    input  logic clock_i,  // Destination clock
    input  logic data_i,   // Input data in source clock domain
    output logic data_o    // Synchronized output in destination clock domain
);
    initial begin
        data_o = INITAL_DATA_O;
    end

    logic q = INITAL_DATA_I;  // 1st stage FF output

    always_ff @(posedge clock_i) begin
        {data_o, q} <= {q, data_i};
    end
endmodule
