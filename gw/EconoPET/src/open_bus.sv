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
 * Open bus emulation for unmapped address regions.
 *
 * On a real Commodore PET, reading an unmapped address returns the last byte
 * that was on the CPU data bus.  This includes bytes from instruction fetches,
 * operand reads, and data writes -- any completed bus cycle leaves its data
 * value on the bus capacitance.
 *
 * This module captures cpu_data_i on every completed bus cycle (signaled by
 * cpu_data_strobe_i) and outputs the captured value when unmapped_i is asserted
 * during a CPU read.
 *
 * See: docs/dev/PET/compat.md for background on open bus behavior.
 */
module open_bus (
    input  logic                  sys_clock_i,
    input  logic                  cpu_data_strobe_i,
    input  logic [DATA_WIDTH-1:0] cpu_data_i,

    input  logic                  unmapped_i,
    input  logic                  cpu_be_i,
    input  logic                  cpu_we_i,

    output logic [DATA_WIDTH-1:0] data_o,
    output logic                  data_oe
);
    logic [DATA_WIDTH-1:0] last_bus_data;

    always_ff @(posedge sys_clock_i) begin
        if (cpu_data_strobe_i) begin
            last_bus_data <= cpu_data_i;
        end
    end

    assign data_o  = last_bus_data;
    assign data_oe = unmapped_i && cpu_be_i && !cpu_we_i;
endmodule
