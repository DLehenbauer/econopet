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

// Scratch file for ad hoc manual testing/experiments.
// Do not include in EconoPET.xml.
module scratch;
    int i;

    initial begin
        for (i = 0; i < 10; i = i + 1) begin
            $display("bit_width(%d) = %d", i, common_pkg::bit_width(i));
        end
        $finish;
    end
endmodule
