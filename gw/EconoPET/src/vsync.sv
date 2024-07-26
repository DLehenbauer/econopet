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

// Simple v-sync generator @60 Hz.
module vsync(
    input  logic cpu_clock_i,
    output logic v_sync_o
);
    logic [14:0] count = 0;

    initial begin
        v_sync_o = 1;
    end

    always_ff @(posedge cpu_clock_i) begin
        // Approximates the timing of a 8032:
        //   60.06 Hz, 95.2% duty cycle (800us negative pulse)
        if (count == 15847) v_sync_o = 0;
        if (count == 16647) begin
            count <= 0;
            v_sync_o = 1;
        end else begin
            count <= count + 1'd1;
        end
    end
endmodule
