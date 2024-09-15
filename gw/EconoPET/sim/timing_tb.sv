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

`include "./sim/assert.svh"
`include "./src/common_pkg.svh"

import common_pkg::*;

module timing_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic cpu_grant;
    logic video_grant;
    logic spi_grant;
    logic strobe;

    timing timing (
        .clock_i(clock),
        .cpu_grant_o(cpu_grant),
        .video_grant_o(video_grant),
        .spi_grant_o(spi_grant),
        .strobe_o(strobe)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        repeat (1024) @(posedge clock);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
