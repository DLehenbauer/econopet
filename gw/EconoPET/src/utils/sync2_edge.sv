
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

 module sync2_edge_detect(
    input  logic clk_i,     // Destination/sampling clock
    input  logic data_i,    // Input data in source clock domain
    output logic data_o,    // Synchronized output in destination clock domain
    output logic pe_o,      // Pulse for rising edge in destination clock domain
    output logic ne_o       // Pulse for falling edge in destination clock domain
);
    sync2 sync_valid(
        .clk_i(clk_i),
        .data_i(data_i),
        .data_o(data_o)
    );

    edge_detect sync2_edge_detect(
        .clk_i(clk_i),
        .data_i(data_o),
        .pe_o(pe_o),
        .ne_o(ne_o)
    );
endmodule
