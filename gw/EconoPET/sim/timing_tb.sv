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

    logic cpu_be;
    logic cpu_clock;
    logic cpu_wr_strobe;
    logic load_sr1;
    logic load_sr2;
    logic clk8_en;
    logic clk16_en;
    logic [0:0] grant;
    logic grant_valid;

    timing timing (
        .sys_clock_i(clock),
        .cpu_be_o(cpu_be),
        .cpu_clock_o(cpu_clock),
        .cpu_wr_strobe_o(cpu_wr_strobe),
        .load_sr1_o(load_sr1),
        .load_sr2_o(load_sr2),
        .clk8_en_o(clk8_en),
        .clk16_en_o(clk16_en),
        .grant_o(grant),
        .grant_valid_o(grant_valid)
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

        @(posedge load_sr2);
        stopwatch.start();
        @(posedge load_sr2);
        $display("[%t] load_sr2 at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge load_sr1);
        stopwatch.start();
        @(posedge load_sr1);
        $display("[%t] load_sr1 at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge cpu_be);
        stopwatch.start();
        @(posedge cpu_be);
        $display("[%t] cpu_be at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge cpu_clock);
        stopwatch.start();
        @(posedge cpu_clock);
        $display("[%t] cpu_clock at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge cpu_wr_strobe);
        stopwatch.start();
        @(posedge cpu_wr_strobe);
        $display("[%t] cpu_wr_strobe at %0.2f mHz", $time, stopwatch.freq_mhz());

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
