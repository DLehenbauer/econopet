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

module top_tb;
    logic cpu_clock;
    logic cpu_reset_n;
    logic cpu_ready;

    mock_system mock_system (
        .cpu_clock_o(cpu_clock),
        .cpu_reset_n_o(cpu_reset_n),
        .cpu_ready_o(cpu_ready)
    );

    task static test_rw(
        input logic [RAM_ADDR_WIDTH-1:0] addr_i,
        input logic [    DATA_WIDTH-1:0] data_i
    );
        logic [DATA_WIDTH-1:0] dout;
        mock_system.wb_write_at(addr_i, data_i);
        mock_system.wb_read_at(addr_i, dout);
        `assert_equal(dout, data_i);
    endtask

    task static usb_keyboard_test;
        bit   [KBD_ADDR_WIDTH-1:0] row;
        logic [    DATA_WIDTH-1:0] dout;
        bit   [    DATA_WIDTH-1:0] value;

        for (row = 0; row < KBD_ROW_COUNT; row = row + 1) begin
            value = { 4'b1011, row };
            mock_system.wb_write_at(common_pkg::wb_kbd_addr(row), { 4'b1011, row });
            $display("[%t]   WB Keyboard[%d] <- %02x", $time, row, value);

            mock_system.cpu_write(16'hE810 + PIA_PORTA, value);
            mock_system.cpu_read(16'hE810 + PIA_PORTB, dout);
            `assert_equal(dout, value);

            $display("[%t]   Keyboard[%d] -> %02x", $time, row, dout);

            // Keyboard interception should not interfere with RAM access.
            test_rw(common_pkg::wb_ram_addr(17'h0E810 + PIA_PORTA), ~value);
            test_rw(common_pkg::wb_ram_addr(17'h0E810 + PIA_PORTB), ~value);
            
            // Writing to RAM should not interfere with keyboard interception.
            mock_system.cpu_read(16'hE810 + PIA_PORTB, dout);
            `assert_equal(dout, value);
        end
    endtask

    task static run;
        logic [DATA_WIDTH-1:0] cpu_dout;
        int count;

        $display("[%t] BEGIN %m", $time);

        mock_system.init;

        usb_keyboard_test;

        mock_system.rom_init;
        mock_system.cpu_start;

        $display("[%t]   CPU must be in RESET / not READY state", $time);
        `assert_equal(cpu_ready, 1'b0);
        `assert_equal(cpu_reset_n, 1'b0);

        $display("[%t]   Perform CPU reset", $time);
        mock_system.wb_write_at(common_pkg::wb_reg_addr(0), 8'b0000_0010);
        `assert_equal(cpu_ready, 1'b0);
        `assert_equal(cpu_reset_n, 1'b0);
        // Verilog-6502 requires two cycles to reset.
        @(cpu_clock);
        @(cpu_clock);

        $display("[%t]   Start CPU", $time);
        mock_system.wb_write_at(common_pkg::wb_reg_addr(0), 8'b0000_0001);
        `assert_equal(cpu_ready, 1'b1);
        `assert_equal(cpu_reset_n, 1'b1);

        mock_system.wb_read_at(16'h8000, cpu_dout);
        for (count = 0; count <= 2000; count = count + 1) begin
            mock_system.wb_read(cpu_dout);
        end

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
