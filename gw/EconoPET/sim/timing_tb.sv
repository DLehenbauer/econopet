// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

module timing_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    stopwatch stopwatch();

    logic clk16_en;
    logic clk8_en;
    logic cpu_be;
    logic cpu_addr_strobe;
    logic cpu_clock;
    logic soft_cpu_clock;
    logic cpu_data_strobe;
    logic cpu_data_en;
    logic cpu_hold_strobe;
    logic cpu_wr_en;
    logic load_sr1;
    logic load_sr2;
    logic [0:0] grant;
    logic grant_valid;

    timing timing (
        .sys_clock_i(clock),
        .clk16_en_o(clk16_en),
        .clk8_en_o(clk8_en),
        .cpu_be_o(cpu_be),
        .cpu_clock_o(cpu_clock),
        .soft_cpu_clock_o(soft_cpu_clock),
        .cpu_addr_strobe_o(cpu_addr_strobe),
        .cpu_data_strobe_o(cpu_data_strobe),
        .cpu_data_en_o(cpu_data_en),
        .cpu_hold_strobe_o(cpu_hold_strobe),
        .cpu_wr_en_o(cpu_wr_en),
        .load_sr1_o(load_sr1),
        .load_sr2_o(load_sr2),
        .grant_o(grant),
        .grant_valid_o(grant_valid)
    );

    task run;
        realtime phi2_fall_time;

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

        @(posedge cpu_be);
        stopwatch.start();
        @(posedge cpu_be);
        $display("[%t] cpu_be at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge cpu_clock);
        stopwatch.start();
        @(posedge cpu_clock);
        $display("[%t] cpu_clock at %0.2f mHz", $time, stopwatch.freq_mhz());
        `assert_equal(soft_cpu_clock, 1'b1)

        // 'cpu_data_en' brackets the window where the FPGA may drive write data on
        // behalf of a soft CPU core, mirroring a physical 6502's data pin timing.
        // The checks below sample on 'negedge clock' (mid-cycle, after the registered
        // output has settled) to pin down both edges of that window.

        // Still high-Z at rising PHI2: a real CPU needs tMDS before its data is valid.
        @(negedge clock);
        `assert_equal(cpu_data_en, 1'b0)

        @(posedge cpu_addr_strobe);
        stopwatch.start();
        @(posedge cpu_addr_strobe);
        $display("[%t] cpu_addr_strobe at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge cpu_data_strobe);
        stopwatch.start();
        @(posedge cpu_data_strobe);
        $display("[%t] cpu_data_strobe at %0.2f mHz", $time, stopwatch.freq_mhz());

        // Opens at the data-valid point (cpu_data_strobe), not earlier at cpu_be.
        @(negedge clock);
        `assert_equal(cpu_data_en, 1'b1)

        // Outlives cpu_wr_en: releasing at WE's rising edge would race the SRAM latch.
        @(negedge cpu_wr_en);
        `assert_equal(cpu_data_en, 1'b1)

        // Outlives PHI2: the PIA/VIA latch on the falling edge and need hold time.
        @(negedge cpu_clock);
        `assert_equal(cpu_data_en, 1'b1)
        `assert_equal(soft_cpu_clock, 1'b1)

        // Soft cores advance only after the external PIA/VIA hold interval.
        phi2_fall_time = $realtime;
        @(negedge soft_cpu_clock);
        assert ($realtime - phi2_fall_time >= IO_tHOLD)
            else $fatal(1, "Soft CPU clock followed PHI2 by only %.3fns (requires %0dns)",
                $realtime - phi2_fall_time, IO_tHOLD);

        // Closes with cpu_be, leaving tBVD for the bus to return to high-Z.
        @(negedge clock);
        `assert_equal(cpu_be, 1'b0)
        `assert_equal(cpu_data_en, 1'b0)

        @(posedge cpu_hold_strobe);
        stopwatch.start();
        @(posedge cpu_hold_strobe);
        $display("[%t] cpu_hold_strobe at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge cpu_hold_strobe);
        `assert_equal(cpu_be, 1'b1)
        @(posedge clock);
        @(negedge clock);
        `assert_equal(cpu_be, 1'b0)

        @(posedge load_sr1);
        stopwatch.start();
        @(posedge load_sr1);
        $display("[%t] load_sr1 at %0.2f mHz", $time, stopwatch.freq_mhz());

        @(posedge load_sr2);
        stopwatch.start();
        @(posedge load_sr2);
        $display("[%t] load_sr2 at %0.2f mHz", $time, stopwatch.freq_mhz());

    endtask

    `TB_INIT
endmodule
