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

import common_pkg::*;

module timing (
    input  logic clock_i,
    output logic cpu_grant_o,
    output logic video_grant_o,
    output logic spi_grant_o,
    output logic strobe_o
);
    logic [5:0] cycle_count = '0;

    always_ff @(posedge clock_i) begin
        cycle_count <= cycle_count + 1'b1;
    end

    localparam CPU_1   = 3'b000,
               CPU_2   = 3'b001,
               VIDEO_1 = 3'b010,
               SPI_1   = 3'b011,
               VIDEO_2 = 3'b100,
               VIDEO_3 = 3'b101,
               VIDEO_4 = 3'b110,
               SPI_2   = 3'b111;

    logic [2:0] grant = '0;

    always_ff @(posedge clock_i) begin
        strobe_o <= '0;
        
        if (cycle_count == '0) begin
            strobe_o <= !(grant == CPU_1);
            grant    <= grant + 1'b1;
        end
    end

    assign cpu_grant_o   = grant == CPU_1   || grant == CPU_2;
    assign video_grant_o = grant == VIDEO_1 || grant == VIDEO_2 || grant == VIDEO_3 || grant == VIDEO_4;
    assign spi_grant_o   = grant == SPI_1   || grant == SPI_2;
endmodule
