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

    logic clk1n_en;
    logic clk2n_en;
    logic clk8_en;
    logic clk16_en;
    logic wb_grant;

    timing timing (
        .sys_clock_i(clock),
        .clk1n_en_o(clk1n_en),
        .clk2n_en_o(clk2n_en),
        .clk8_en_o(clk8_en),
        .clk16_en_o(clk16_en),
        .wb_grant_o(wb_grant)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        @(posedge clock);
        stopwatch.start();
        @(posedge clock);
        $display("[%t] sys_clock at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge clk16_en);
        stopwatch.start();
        @(posedge clk16_en);
        $display("[%t] clk16_en at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge clk8_en);
        stopwatch.start();
        @(posedge clk8_en);
        $display("[%t] clk8_en at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge clk2n_en);
        stopwatch.start();
        @(posedge clk2n_en);
        $display("[%t] clk1n_en at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge clk1n_en);
        stopwatch.start();
        @(posedge clk1n_en);
        $display("[%t] clk1n_en at %0.2f mHz", $time, stopwatch.freq_mhz());

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
