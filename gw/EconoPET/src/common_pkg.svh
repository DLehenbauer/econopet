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
    // There are three places clock frequencies are defined, which must be kept in sync:
    //
    //   1.  Here
    //   2.  In the *.sdc
    //   3.  In the interface designer (*.peri.xml)
    //
    localparam integer unsigned SYS_CLOCK_MHZ = 64;
    localparam integer unsigned SPI_SCK_MHZ = 24;

    localparam integer unsigned WB_ADDR_WIDTH = 20;
    localparam integer unsigned RAM_ADDR_WIDTH = 17;
    localparam integer unsigned CPU_ADDR_WIDTH = 16;
    localparam integer unsigned DATA_WIDTH = 8;
endpackage

`endif
