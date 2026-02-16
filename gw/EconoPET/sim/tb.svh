// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`ifndef TB_SVH
`define TB_SVH

//`define TRACE

// Note: Macro defined on a single line to prevent line numbers from changing during expansion [iverilog 12]
`define assert_compare(ACTUAL, OP, EXPECTED) assert(ACTUAL OP EXPECTED) begin `ifdef TRACE $info("[%t] %m.ACTUAL=%0d ($%x %b)", $time, ACTUAL, ACTUAL, ACTUAL); `endif end else begin $fatal(1, "[%0t] %m expected 'ACTUAL=%0d ($%x %b)', but got 'ACTUAL=%0d ($%x %b)'.", $time, EXPECTED, EXPECTED, EXPECTED, ACTUAL, ACTUAL, ACTUAL); end
`define assert_equal(ACTUAL, EXPECTED) `assert_compare(ACTUAL, ==, EXPECTED)
`define assert_exact_equal(ACTUAL, EXPECTED) `assert_compare(ACTUAL, ===, EXPECTED)

`define TB_INIT initial begin $dumpfile($sformatf("work_sim/%m.vcd")); $dumpvars(0); $display("[%t] BEGIN %m", $time); run; #1 $display("[%t] END %m", $time); $finish; end

`endif
