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

/**
 * CPU data bus multiplexer for FPGA-driven sources.
 *
 * Selects one of multiple FPGA data sources to drive the CPU data bus.
 * Each source provides a data value and output-enable signal. The OE signals
 * must be mutually exclusive (at most one active at a time), which is enforced
 * by an assertion in simulation.
 *
 * This mux handles only FPGA drivers; external SRAM drives the bus directly
 * via ram_oe_o and is not included here.
 *
 * @param COUNT Number of data sources
 */
module cpu_data_mux #(
    parameter COUNT = 3   // Number of data sources
) (
    // Data sources (active-high output enable)
    input  logic [COUNT-1:0][DATA_WIDTH-1:0] data_i,    // Data from each source
    input  logic [COUNT-1:0]                 oe_i,      // Output enable from each source

    // Muxed output to CPU data bus
    output logic [DATA_WIDTH-1:0] data_o,   // Selected data value
    output logic                  oe_o      // Output enable (active when any source enabled)
);
    // Binary index corresponding to the one-hot position
    logic [$clog2(COUNT)-1:0] sel_index;

    // Convert the one-hot OE signal to binary index
    always_comb begin
        sel_index = '0;

        for (int i = 0; i < COUNT; i++) begin
            if (oe_i[i]) begin
                sel_index = i[$bits(sel_index)-1:0];
            end
        end
    end

    // Output data from selected source, or don't-care when no source active
    assign data_o = |oe_i ? data_i[sel_index] : 'x;
    assign oe_o   = |oe_i;

    // synthesis off
    always_comb begin
        // At most one OE may be active at any time
        assert ($onehot0(oe_i)) else $fatal(1, "cpu_data_mux: multiple OE signals active");
    end
    // synthesis on
endmodule
