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

import common_pkg::*;

module wb_decode #(
    parameter WB_ADDR_BASE = 1'd0
) (
    input  logic [WB_ADDR_WIDTH-1:0] wb_addr_i,
    output logic selected_o
);
    assign selected_o = wb_addr_i[WB_ADDR_WIDTH-1:WB_ADDR_WIDTH-$bits(WB_ADDR_BASE)] == WB_ADDR_BASE;
endmodule
