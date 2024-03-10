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

`timescale 1ns / 1ps

module clock_gen #(
    parameter MHZ = 24    // Clock speed in MHz
) (
    output logic clock_o  // Destination clock
);
    localparam PERIOD = 1000 / MHZ;

    bit enable = '0;

    always @(posedge enable) begin
        while (enable) begin
            #(PERIOD / 4);
            clock_o <= 1'b1;
            #(PERIOD / 2);
            clock_o <= '0;
            #(PERIOD / 4);
        end
    end

    task start;
        clock_o = '0;
        enable  = 1'b1;
    endtask

    task stop;
        enable = '0;
    endtask
endmodule
