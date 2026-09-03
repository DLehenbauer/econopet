// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`ifndef TB_SVH
`define TB_SVH

//`define TRACE

// Note: Macro defined on a single line to prevent line numbers from changing during expansion [iverilog 12]
`define assert_compare(ACTUAL, OP, EXPECTED) assert(ACTUAL OP EXPECTED) begin `ifdef TRACE $info("[%t] %m.ACTUAL=%0d ($%x %b)", $time, ACTUAL, ACTUAL, ACTUAL); `endif end else begin $fatal(1, "[%0t] %m expected 'ACTUAL=%0d ($%x %b)', but got 'ACTUAL=%0d ($%x %b)'.", $time, EXPECTED, EXPECTED, EXPECTED, ACTUAL, ACTUAL, ACTUAL); end
`define assert_equal(ACTUAL, EXPECTED) `assert_compare(ACTUAL, ==, EXPECTED)
`define assert_exact_equal(ACTUAL, EXPECTED) `assert_compare(ACTUAL, ===, EXPECTED)

// Common top-level pins that are intentionally unused by gateware testbenches.
`define TOP_UNUSED_PORTS .status_no(), .cpu_irq_n_o(), .cpu_irq_n_oe(), .cpu_nmi_n_o(), .cpu_nmi_n_oe(), .spi1_cs_ni(1'b1), .spi1_sck_i(1'b0), .spi1_sd_i(1'b0), .spi1_sd_o(), .horiz_drive_o(), .vert_drive_o(), .jiffy_clock_o(), .video_o(), .diag_i(1'b0), .via_cb2_i(1'b0), .audio_o(), .audio_det_n_i(1'b1), .pmod1_i('0), .pmod1_o(), .pmod1_oe(), .pmod2_i('0), .pmod2_o(), .pmod2_oe(), .sp1_i(1'b0), .sp1_o(), .sp1_oe(), .sp2_i(1'b0), .sp2_o(), .sp2_oe(), .sp3_i(1'b0), .sp3_o(), .sp3_oe(), .sp4_i(1'b0), .sp4_o(), .sp4_oe(), .sp5_i(1'b0), .sp5_o(), .sp5_oe(), .sp6_i(1'b0), .sp6_o(), .sp6_oe(), .sp7_i(1'b0), .sp7_o(), .sp7_oe(), .sp8_i(1'b0), .sp8_o(), .sp8_oe(),

// Note: Macros defined on a single line to prevent line numbers from changing during expansion [iverilog 12]
// Icarus has no global RNG seed, so seed $urandom from the +seed plusarg supplied by sim.sh/verilate.sh.
`define TB_INIT int tb_seed; initial begin if ($value$plusargs("seed=%d", tb_seed)) void'($urandom(tb_seed)); $dumpfile($sformatf("work_sim/%m.vcd")); $dumpvars(0); $display("[%t] BEGIN %m", $time); run; #1 $display("[%t] END %m", $time); $finish; end

`endif
