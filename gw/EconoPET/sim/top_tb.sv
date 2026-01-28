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

    task static spi_ram_test;
        integer i;
        bit   [WB_ADDR_WIDTH-2:0] addr;        // Constrain to RAM address space $0000-$7FFF
        logic [   DATA_WIDTH-1:0] dout;
        bit   [   DATA_WIDTH-1:0] value;

        $display("[%t] Begin SPI/RAM Test", $time);

        mock_system.spi_read_at(common_pkg::wb_ram_addr(17'h1ffff), dout);

        for (i = 0; i < 10; i = i + 1) begin
            addr = $random();
            value = $random();
            mock_system.spi_write_at(common_pkg::wb_ram_addr(addr), value);
            mock_system.spi_read_at(common_pkg::wb_ram_addr(addr), dout);
            `assert_equal(dout, value);
        end

        $display("[%t] End SPI/RAM Test", $time);
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
            mock_system.spi_write_at(common_pkg::wb_kbd_addr(col), value);

            // Read from CPU
            mock_system.cpu_write(16'hE810 + PIA_PORTA, value);
            mock_system.cpu_read(16'hE810 + PIA_PORTB, dout);
            $display("[%t]   Keyboard[%0d] -> %02x (CPU)", $time, col, dout);
            `assert_equal(dout, value);

            // Value read by CPU should be available via WB
            mock_system.spi_read_at(common_pkg::wb_kbd_addr(col), dout);
            $display("[%t]   Keyboard[%0d] -> %02x (WB)", $time, col, dout);
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

        // Test 1: Write CRTC registers via CPU, verify via SPI wishbone
        $display("[%t]   Test 1: CPU writes, SPI reads", $time);
        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            mock_system.cpu_write(16'hE880, r);
            value = { 3'b101, r[4:0] };
            mock_system.cpu_write(16'hE881, value);
            $display("[%t]     CRTC[%0d] <- %02x (CPU)", $time, r, value);

            mock_system.spi_read_at(common_pkg::wb_crtc_addr(r), dout);
            `assert_equal(dout, value);
        end

        // Test 2: Write CRTC registers via SPI wishbone, verify via SPI readback
        $display("[%t]   Test 2: SPI writes, SPI reads", $time);
        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            // Use different value pattern to distinguish from Test 1
            value = { 3'b110, r[4:0] };
            mock_system.spi_write_at(common_pkg::wb_crtc_addr(r), value);
            $display("[%t]     CRTC[%0d] <- %02x (SPI)", $time, r, value);

            mock_system.spi_read_at(common_pkg::wb_crtc_addr(r), dout);
            `assert_equal(dout, value);
        end

        $display("[%t] End CRTC Write Test", $time);
    endtask

    task static crtc_read_test;
        integer r;
        logic [DATA_WIDTH-1:0] dout;
        logic [DATA_WIDTH-1:0] masked;

        $display("[%t] Begin CRTC Read Test", $time);

        // Test 1: CPU reads from CRTC status register ($E880, RS=0).
        // Status register format:
        //   Bit 7:   Not used
        //   Bit 6:   LPEN Register Full (always 0, no light pen support)
        //   Bit 5:   Vertical Blanking (1 = in vblank, tested in video_crtc_tb.sv)
        //   Bit 4-0: Not used
        $display("[%t]   Test 1: Status register read", $time);
        mock_system.cpu_read(16'hE880, dout);
        $display("[%t]   CRTC Status -> %02x (CPU)", $time, dout);
        // VBlank (bit 5) is tested in `video_crtc_tb.sv`.  Here we just sanity
        // check that the unused bits return zero.
        masked = dout & 8'b1001_1111;
        `assert_equal(masked, 8'h00);

        // Test 2: CPU reads from CRTC data register ($E881, RS=1) should return 0.
        // Real MOS6545 CRTC only supports reading R14-R17 (cursor/light pen).  Our
        // implementation returns 0 for all registers.
        $display("[%t]   Test 2: Data register reads", $time);
        for (r = 0; r < CRTC_REG_COUNT; r = r + 1) begin
            mock_system.cpu_write(16'hE880, r);         // Select register r
            mock_system.cpu_read(16'hE881, dout);       // Read from CRTC data register
            $display("[%t]     CRTC[%0d] -> %02x (CPU)", $time, r, dout);
            `assert_equal(dout, 8'h00);                 // Expect 0 for all reads
        end

        // Test 3: Verify CRTC read does not interfere with IO reads.
        // After CRTC read, keyboard should still work correctly.
        $display("[%t]   Test 3: CRTC/IO mutual exclusion", $time);
        mock_system.spi_write_at(common_pkg::wb_kbd_addr(0), 8'hA5);
        mock_system.cpu_write(16'hE810 + PIA_PORTA, 8'h00);  // Select column 0
        mock_system.cpu_read(16'hE810 + PIA_PORTB, dout);
        `assert_equal(dout, 8'hA5);

        $display("[%t] End CRTC Read Test", $time);
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

    task static bram_test;
        integer i;
        bit   [BRAM_ADDR_WIDTH-1:0] addr;
        logic [     DATA_WIDTH-1:0] dout;
        bit   [     DATA_WIDTH-1:0] value;

        // Calculate last valid BRAM address
        localparam bit [BRAM_ADDR_WIDTH-1:0] BRAM_LAST_ADDR = (1 << BRAM_ADDR_WIDTH) - 1;

        // Boundary test addresses: 0, 1, 2, n-2, n-1, n
        localparam int NUM_BOUNDARY_TESTS = 6;
        bit [BRAM_ADDR_WIDTH-1:0] test_addrs[NUM_BOUNDARY_TESTS];
        bit [DATA_WIDTH-1:0] test_values[NUM_BOUNDARY_TESTS];

        $display("[%t] Begin BRAM Test", $time);

        test_addrs[0] = BRAM_ADDR_WIDTH'(0);
        test_addrs[1] = BRAM_ADDR_WIDTH'(1);
        test_addrs[2] = BRAM_ADDR_WIDTH'(2);
        test_addrs[3] = BRAM_LAST_ADDR - 2;
        test_addrs[4] = BRAM_LAST_ADDR - 1;
        test_addrs[5] = BRAM_LAST_ADDR;

        test_values[0] = $random();
        test_values[1] = $random();
        test_values[2] = $random();
        test_values[3] = $random();
        test_values[4] = $random();
        test_values[5] = $random();

        // Write addresses at/near boundaries
        for (i = 0; i < NUM_BOUNDARY_TESTS; i = i + 1) begin
            $display("[%t]   Writing BRAM[0x%03x] <- 0x%02x", $time, test_addrs[i], test_values[i]);
            mock_system.spi_write_at(common_pkg::wb_bram_addr(test_addrs[i]), test_values[i]);
        end

        // Verify previous writes
        for (i = 0; i < NUM_BOUNDARY_TESTS; i = i + 1) begin
            mock_system.spi_read_at(common_pkg::wb_bram_addr(test_addrs[i]), dout);
            $display("[%t]   Verifying BRAM[0x%03x] == 0x%02x", $time, test_addrs[i], test_values[i]);
            `assert_equal(dout, test_values[i]);
        end

        // Test random addresses
        for (i = 0; i < 10; i = i + 1) begin
            addr = $random();
            value = $random();
            $display("[%t]   BRAM[0x%03x] <- 0x%02x", $time, addr, value);
            mock_system.spi_write_at(common_pkg::wb_bram_addr(addr), value);
            mock_system.spi_read_at(common_pkg::wb_bram_addr(addr), dout);
            `assert_equal(dout, value);
        end

        $display("[%t] End BRAM Test", $time);
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
        // mock_system.ram_fill(17'h08000, 17'h087ff, 8'd66);      // Fill VRAM with fine checkerboard pattern

        spi_ram_test;
        cpu_ram_test;
        usb_keyboard_test;
        crtc_write_test;
        crtc_read_test;
        sid_write_test;
        register_file_test;
        bram_test;

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
