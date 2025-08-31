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

import common_pkg::*;

module memory_map_tb();
    bit[WB_ADDR_WIDTH-1:0] expected_addr;

    task check_next(input string name, input bit[19:0] addr);
        $display("[%t]   %-20s : %05x", $time, name, addr);
        `assert_equal(addr, expected_addr);
        expected_addr = expected_addr + 1'b1;;
    endtask

    task run;
        $display("[%t] BEGIN %m", $time);

        // expected_addr = common_pkg::wb_io_addr(REG_IO_VIA_PORTB);
        // check_next("REG_IO_VIA_PORTB", common_pkg::wb_io_addr(REG_IO_VIA_PORTB));
        // check_next("REG_IO_VIA_PORTA", common_pkg::wb_io_addr(REG_IO_VIA_PORTA));
        // check_next("REG_IO_VIA_DDRB", common_pkg::wb_io_addr(REG_IO_VIA_DDRB));
        // check_next("REG_IO_VIA_DDRA", common_pkg::wb_io_addr(REG_IO_VIA_DDRA));
        // check_next("REG_IO_VIA_T1CL", common_pkg::wb_io_addr(REG_IO_VIA_T1CL));
        // check_next("REG_IO_VIA_T1CH", common_pkg::wb_io_addr(REG_IO_VIA_T1CH));
        // check_next("REG_IO_VIA_T1LL", common_pkg::wb_io_addr(REG_IO_VIA_T1LL));
        // check_next("REG_IO_VIA_T1LH", common_pkg::wb_io_addr(REG_IO_VIA_T1LH));
        // check_next("REG_IO_VIA_T2CL", common_pkg::wb_io_addr(REG_IO_VIA_T2CL));
        // check_next("REG_IO_VIA_T2CH", common_pkg::wb_io_addr(REG_IO_VIA_T2CH));
        // check_next("REG_IO_VIA_SR", common_pkg::wb_io_addr(REG_IO_VIA_SR));
        // check_next("REG_IO_VIA_ACR", common_pkg::wb_io_addr(REG_IO_VIA_ACR));
        // check_next("REG_IO_VIA_PCR", common_pkg::wb_io_addr(REG_IO_VIA_PCR));
        // check_next("REG_IO_VIA_IFR", common_pkg::wb_io_addr(REG_IO_VIA_IFR));
        // check_next("REG_IO_VIA_IER", common_pkg::wb_io_addr(REG_IO_VIA_IER));
        // check_next("REG_IO_VIA_ORA", common_pkg::wb_io_addr(REG_IO_VIA_ORA));
        // check_next("REG_IO_PIA1_PORTA", common_pkg::wb_io_addr(REG_IO_PIA1_PORTA));
        // check_next("REG_IO_PIA1_CRA", common_pkg::wb_io_addr(REG_IO_PIA1_CRA));
        // check_next("REG_IO_PIA1_PORTB", common_pkg::wb_io_addr(REG_IO_PIA1_PORTB));
        // check_next("REG_IO_PIA1_CRB", common_pkg::wb_io_addr(REG_IO_PIA1_CRB));
        // check_next("REG_IO_PIA2_PORTA", common_pkg::wb_io_addr(REG_IO_PIA2_PORTA));
        // check_next("REG_IO_PIA2_CRA", common_pkg::wb_io_addr(REG_IO_PIA2_CRA));
        // check_next("REG_IO_PIA2_PORTB", common_pkg::wb_io_addr(REG_IO_PIA2_PORTB));
        // check_next("REG_IO_PIA2_CRB", common_pkg::wb_io_addr(REG_IO_PIA2_CRB));
        // check_next("IO_REG_COUNT", common_pkg::wb_io_addr(IO_REG_COUNT));

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
