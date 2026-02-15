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

`ifndef TB_SVH
`define TB_SVH

//`define TRACE

// Note: Macro defined on a single line to prevent line numbers from changing during expansion [iverilog 12]
`define assert_compare(ACTUAL, OP, EXPECTED) assert(ACTUAL OP EXPECTED) begin `ifdef TRACE $info("[%t] %m.ACTUAL=%0d ($%x %b)", $time, ACTUAL, ACTUAL, ACTUAL); `endif end else begin $fatal(1, "[%0t] %m expected 'ACTUAL=%0d ($%x %b)', but got 'ACTUAL=%0d ($%x %b)'.", $time, EXPECTED, EXPECTED, EXPECTED, ACTUAL, ACTUAL, ACTUAL); end
`define assert_equal(ACTUAL, EXPECTED) `assert_compare(ACTUAL, ==, EXPECTED)
`define assert_exact_equal(ACTUAL, EXPECTED) `assert_compare(ACTUAL, ===, EXPECTED)

`define TB_INIT initial begin $dumpfile($sformatf("work_sim/%m.vcd")); $dumpvars(0); $display("[%t] BEGIN %m", $time); run; #1 $display("[%t] END %m", $time); $finish; end

`endif
