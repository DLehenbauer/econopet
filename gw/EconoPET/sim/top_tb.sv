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
        mock_system.spi_write_at(addr_i, data_i);
        mock_system.spi_read_at(addr_i, dout);
        `assert_equal(dout, data_i);
    endtask

    task static cpu_ram_test;
        integer i;
        bit   [CPU_ADDR_WIDTH-2:0] addr;        // Constrain to RAM address space $0000-$7FFF
        logic [    DATA_WIDTH-1:0] dout;
        bit   [    DATA_WIDTH-1:0] value;

        $display("[%t] Begin CPU/RAM Test", $time);

        for (i = 0; i < 10; i = i + 1) begin
            addr = $random();
            value = $random();
            mock_system.cpu_write({ 1'b0, addr }, value);
            mock_system.cpu_read({ 1'b0, addr }, dout);
            `assert_equal(dout, value);
        end

        $display("[%t] End CPU/RAM Test", $time);
    endtask

    task static usb_keyboard_test;
        integer col;
        logic [DATA_WIDTH-1:0] dout;
        bit   [DATA_WIDTH-1:0] value;

        $display("[%t] Begin USB Keyboard Test", $time);

        for (col = 0; col < KBD_COL_COUNT; col = col + 1) begin
            value = { 4'b1011, col[3:0] };
            
            $display("[%t]   Keyboard[%0d] <- %02x (WB)", $time, col, value);
            mock_system.spi_write_at(common_pkg::wb_io_kbd_addr(col), value);

            mock_system.spi_read_at(common_pkg::wb_io_kbd_addr(col), dout);
            $display("[%t]   Keyboard[%0d] -> %02x (WB)", $time, col, dout);
            `assert_equal(dout, value);

            mock_system.cpu_write(16'hE810 + PIA_PORTA, value);
            mock_system.cpu_read(16'hE810 + PIA_PORTB, dout);
            $display("[%t]   Keyboard[%0d] -> %02x (CPU)", $time, col, dout);
            `assert_equal(dout, value);

            // Keyboard interception should not interfere with RAM access.
            test_rw(common_pkg::wb_ram_addr(17'h0E810 + PIA_PORTA), ~value);
            test_rw(common_pkg::wb_ram_addr(17'h0E810 + PIA_PORTB), ~value);
            
            // Writing to RAM should not interfere with keyboard interception.
            mock_system.cpu_read(16'hE810 + PIA_PORTB, dout);
            `assert_equal(dout, value);
        end

        $display("[%t] End USB Keyboard Test", $time);
    endtask

    task static crtc_write_test;
        integer r;
        logic [     DATA_WIDTH-1:0] dout;
        logic [     DATA_WIDTH-1:0] value;

        $display("[%t] Begin CRTC Write Test", $time);

        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            mock_system.cpu_write(16'hE880, r);
            value = { 3'b101, r };
            mock_system.cpu_write(16'hE881, value);
            $display("[%t]   CRTC[%0d] -> %02x", $time, r, value);

            // TODO: Implement read-back from CRTC registers.
            // mock_system.spi_read_at(common_pkg::wb_crtc_addr(r), dout);
            // `assert_equal(dout, value);
        end

        $display("[%t] End CRTC Write Test", $time);
    endtask

    task static sid_write_test;
        integer r;
        logic [     DATA_WIDTH-1:0] dout;
        logic [     DATA_WIDTH-1:0] value;

        $display("[%t] Begin SID Write Test", $time);

        for (r = 0; r < SID_REG_COUNT; r = r + 1) begin
            value = { 3'b101, r };
            mock_system.cpu_write(16'h8F00 + r, value);
            $display("[%t]   SID[%0d] -> %02x", $time, r, value);

            // TODO: Implement read-back from SID registers.
            // mock_system.spi_read_at(common_pkg::wb_SID_addr(r), dout);
            // `assert_equal(dout, value);
        end

        $display("[%t] End SID Write Test", $time);
    endtask

    task static register_file_test;
        logic [     DATA_WIDTH-1:0] dout;

        $display("[%t] Begin RegisterFile Test", $time);

        mock_system.set_config(/* crt */ 0, /* keyboard */ 0);
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_STATUS), dout);
        `assert_equal(dout[REG_STATUS_CRT_BIT], 1'b0);
        `assert_equal(dout[REG_STATUS_KEYBOARD_BIT], 1'b0);

        mock_system.set_config(/* crt */ 1, /* keyboard */ 0);
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_STATUS), dout);
        `assert_equal(dout[REG_STATUS_CRT_BIT], 1'b1);
        `assert_equal(dout[REG_STATUS_KEYBOARD_BIT], 1'b0);

        mock_system.set_config(/* crt */ 0, /* keyboard */ 1);
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_STATUS), dout);
        `assert_equal(dout[REG_STATUS_CRT_BIT], 1'b0);
        `assert_equal(dout[REG_STATUS_KEYBOARD_BIT], 1'b1);

        mock_system.set_config(/* crt */ 1, /* keyboard */ 1);
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_STATUS), dout);
        `assert_equal(dout[REG_STATUS_CRT_BIT], 1'b1);
        `assert_equal(dout[REG_STATUS_KEYBOARD_BIT], 1'b1);

        $display("[%t] End RegisterFile Test", $time);
    endtask

    task static run;
        logic [DATA_WIDTH-1:0] cpu_dout;
        int count;

        $display("[%t] BEGIN %m", $time);

        mock_system.init;
        mock_system.ram_fill(17'h08000, 17'h087ff, 8'd66);      // Fill VRAM with fine checkerboard pattern

        cpu_ram_test;
        usb_keyboard_test;
        crtc_write_test;
        sid_write_test;
        register_file_test;

        mock_system.rom_init;
        mock_system.cpu_start;

        $display("[%t]   CPU must be in RESET / not READY state", $time);
        `assert_equal(cpu_ready, 1'b0);
        `assert_equal(cpu_reset_n, 1'b0);

        $display("[%t]   Perform CPU reset", $time);
        mock_system.spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0010);
        `assert_equal(cpu_ready, 1'b0);
        `assert_equal(cpu_reset_n, 1'b0);
        
        // Like the W65C02S, the Verilog-6502 core requires that RESB be held low for two
        // clock cycles after power on.
        @(cpu_clock);
        @(cpu_clock);

        $display("[%t]   Start CPU", $time);
        mock_system.spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0001);
        `assert_equal(cpu_ready, 1'b1);
        `assert_equal(cpu_reset_n, 1'b1);

        mock_system.spi_read_at(16'h8000, cpu_dout);
        for (count = 0; count <= 25; count = count + 1) begin
            mock_system.spi_read(cpu_dout);
        end

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
