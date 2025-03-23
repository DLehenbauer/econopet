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

//`include "./src/common_pkg.svh"

// Scratch file for ad hoc manual testing/experiments.
// Do not include in EconoPET.xml.


// Module that exposes an unpacked 10x8 array.
module scratch(
    output logic [9:0][7:0] matrix_o
);
    // Initialize the matrix:
    initial begin
        integer col;
        for (col = 0; col < 10; col = col + 1) begin
            matrix_o[col] = '1;
        end
    end
endmodule

module sim;
    // Create an instance of our test module.
    logic [9:0][7:0] matrix_o;

    scratch scratch (
        .matrix_o(matrix_o)
    );

    // Loop through the columns, comparing the internal state of 'matrix_o' to the output.
    initial begin
        integer col;
        logic [7:0] row;

        for (col = 0; col < 10; col = col + 1) begin
            $display("scratch.matrix_o[%0d] = %b, matrix_o[%0d] = %b", col, scratch.matrix_o[col], col, matrix_o[col]);
        end
    end
endmodule
