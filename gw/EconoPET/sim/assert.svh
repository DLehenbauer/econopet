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

`ifndef ASSERT_SVH
`define ASSERT_SVH

//`define TRACE 

// Note: Macros defined on a single line to prevent line numbers from changing during expansion [iverilog 12]
 
`define assert_equal(ACTUAL, EXPECTED) assert(ACTUAL == EXPECTED) begin `ifdef TRACE $info("[%t] %m.ACTUAL=%0d ($%x)", $time, ACTUAL, ACTUAL); `endif end else begin $fatal(1, "[%0t] Expected 'ACTUAL=%0d ($%x)', but got 'ACTUAL=%0d ($%x)'.", $time, EXPECTED, EXPECTED, ACTUAL, ACTUAL); end
 
`endif
