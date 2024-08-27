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

`ifndef COMMON_PKG_SVH
`define COMMON_PKG_SVH

package common_pkg;
    //
    // Timing
    //

    // There are three places clock frequencies are defined, which must be kept in sync:
    //
    //   1.  Here
    //   2.  In the *.sdc
    //   3.  In the interface designer (*.peri.xml)
    //
    localparam real SYS_CLOCK_MHZ = 64;
    localparam real SPI_SCK_MHZ = 24;

    function real mhz_to_ns(input real freq_mhz);
        return 1000.0 / freq_mhz;
    endfunction

    function int ns_to_cycles(input int time_ns);
        return int'($ceil(time_ns / mhz_to_ns(SYS_CLOCK_MHZ)));
    endfunction

    //
    // Registers
    //

    localparam int unsigned REG_CPU           = 0;
    localparam int unsigned REG_CPU_READY_BIT = 0;
    localparam int unsigned REG_CPU_RESET_BIT = 1;
    localparam int unsigned REG_COUNT         = 1;

    //
    // Bus
    //

    // Calculates the required bit width to store the given value.
    function int bit_width(input int value);
        return $clog2(value + 1'b1);
    endfunction

    localparam int unsigned WB_ADDR_WIDTH  = 20;
    localparam int unsigned RAM_ADDR_WIDTH = 17;
    localparam int unsigned CPU_ADDR_WIDTH = 16;
    localparam int unsigned REG_ADDR_WIDTH = bit_width(REG_COUNT);
    localparam int unsigned DATA_WIDTH     = 8;

    localparam WB_RAM_BASE = 3'b000;
    localparam WB_REG_BASE = 3'b010;

    // TODO: Move these address helpers to ../sim?
    function bit[WB_ADDR_WIDTH-1:0] wb_ram_addr(input bit[RAM_ADDR_WIDTH-1:0] address);
        return { WB_RAM_BASE, address };
    endfunction

    function bit[WB_ADDR_WIDTH-1:0] wb_reg_addr(input bit[REG_ADDR_WIDTH-1:0] register);
        return { WB_REG_BASE, (WB_ADDR_WIDTH - REG_ADDR_WIDTH - $bits(WB_REG_BASE))'('x), register };
    endfunction
endpackage

`endif
