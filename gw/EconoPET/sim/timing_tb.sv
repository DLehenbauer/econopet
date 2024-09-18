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

    stopwatch stopwatch();

    logic cpu_grant;
    logic video_grant;
    logic spi_grant;
    logic strobe;
    logic clk8_en;
    logic clk16_en;

    timing timing (
        .clock_i(clock),
        .cpu_grant_o(cpu_grant),
        .video_grant_o(video_grant),
        .spi_grant_o(spi_grant),
        .strobe_o(strobe),
        .clk8_en_o(clk8_en),
        .clk16_en_o(clk16_en)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        @(posedge clk8_en);
        stopwatch.start();
        @(posedge clk8_en);
        $display("[%t] clk8_en at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge clk16_en);
        stopwatch.start();
        @(posedge clk16_en);
        $display("[%t] clk16_en at %0.2f mHz", $time, stopwatch.freq_mhz());

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
