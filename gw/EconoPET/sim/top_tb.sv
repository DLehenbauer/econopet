// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

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

    task static breakpoint_test;
        logic [DATA_WIDTH-1:0] dout;
        logic [DATA_WIDTH-1:0] addr_lo;
        logic [DATA_WIDTH-1:0] addr_hi;
        integer i;

        $display("[%t] Begin Breakpoint Test", $time);

        // Use the tape buffer ($027A) so the test does not disturb other memory.
        // Write STP ($DB) at $027A and fill $027B-$0289 with NOP ($EA).
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027A), 8'hDB);
        for (i = 1; i < 16; i = i + 1)
            mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027A + i), 8'hEA);

        // Write reset vector ($FFFC/$FFFD) to point to $027A.
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0FFFC), 8'h7A);
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0FFFD), 8'h02);

        // Exit manual mode so the 6502 core drives the bus.
        mock_system.cpu_start;

        // Release reset (bit 1) but keep CPU halted (ready = 0).
        mock_system.spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0010);

        // The Verilog-6502 core requires two clock cycles while RESB is low.
        @(posedge cpu_clock);
        @(posedge cpu_clock);

        // Start CPU: assert ready (bit 0), deassert reset (bit 1 still set).
        mock_system.spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0001);
        `assert_equal(cpu_ready, 1'b1);

        // Wait for the reset sequence (~7 cycles) and the opcode fetch at $027A.
        repeat (20) @(posedge cpu_clock);

        // ---- Verify initial halt ----
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_STATUS), dout);
        $display("[%t]   STATUS after STP = %02x", $time, dout);
        `assert_equal(dout[REG_STATUS_BP_HALT_BIT], 1'b1);

        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_BP_ADDR_LO), addr_lo);
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_BP_ADDR_HI), addr_hi);
        $display("[%t]   Captured address = $%02x%02x", $time, addr_hi, addr_lo);
        `assert_equal(addr_lo, 8'h7A);
        `assert_equal(addr_hi, 8'h02);

        // ---- Patch sequence: INC $DB / STP / DEC $DB / STP ----
        //
        //   $027A: E6 DB     INC $DB       (if CPU resumes here)
        //   $027C: DB        STP           (halt, verify INC effect)
        //   $027D: C6 DB     DEC $DB       (after patching $027C to NOP)
        //   $027F: DB        STP           (halt, verify DEC restored)
        //
        // If the CPU wrongly resumes at $027B instead of $027A, it will
        // execute $DB (STP) immediately and halt at $027B, not $027C.
        //
        // Pre-fill the target zero-page location with a known value.
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h000DB), 8'h41);
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027A), 8'hE6);  // INC zp
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027B), 8'hDB);  // operand $DB (also STP)
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027C), 8'hDB);  // STP
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027D), 8'hC6);  // DEC zp
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027E), 8'hDB);  // operand $DB
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027F), 8'hDB);  // STP

        // ---- Clear breakpoint (CPU resumes INC $DB, then halts at $027C) ----
        mock_system.spi_write_at(common_pkg::wb_reg_addr(REG_BP_CTL), 8'h01);
        repeat (20) @(posedge cpu_clock);

        // ---- Verify halt at $027C (proves CPU resumed at $027A) ----
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_STATUS), dout);
        $display("[%t]   STATUS after INC+STP = %02x", $time, dout);
        `assert_equal(dout[REG_STATUS_BP_HALT_BIT], 1'b1);

        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_BP_ADDR_LO), addr_lo);
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_BP_ADDR_HI), addr_hi);
        $display("[%t]   Halt address = $%02x%02x (expect $027C)", $time, addr_hi, addr_lo);
        `assert_equal(addr_lo, 8'h7C);
        `assert_equal(addr_hi, 8'h02);

        // ---- Verify INC $DB took effect ----
        mock_system.spi_read_at(common_pkg::wb_ram_addr(17'h000DB), dout);
        $display("[%t]   RAM[$00DB] = %02x (expect $42)", $time, dout);
        `assert_equal(dout, 8'h42);

        // ---- Patch STP at $027C to NOP so CPU continues to DEC $DB ----
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027C), 8'hEA);  // NOP

        // ---- Clear breakpoint (CPU resumes NOP, DEC $DB, halts at $027F) ----
        mock_system.spi_write_at(common_pkg::wb_reg_addr(REG_BP_CTL), 8'h01);
        repeat (20) @(posedge cpu_clock);

        // ---- Verify halt at $027F ----
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_STATUS), dout);
        $display("[%t]   STATUS after DEC+STP = %02x", $time, dout);
        `assert_equal(dout[REG_STATUS_BP_HALT_BIT], 1'b1);

        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_BP_ADDR_LO), addr_lo);
        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_BP_ADDR_HI), addr_hi);
        $display("[%t]   Halt address = $%02x%02x (expect $027F)", $time, addr_hi, addr_lo);
        `assert_equal(addr_lo, 8'h7F);
        `assert_equal(addr_hi, 8'h02);

        // ---- Verify DEC $DB restored the original value ----
        mock_system.spi_read_at(common_pkg::wb_ram_addr(17'h000DB), dout);
        $display("[%t]   RAM[$00DB] = %02x (expect $41)", $time, dout);
        `assert_equal(dout, 8'h41);

        // ---- Final cleanup: clear last halt ----
        mock_system.spi_write_at(common_pkg::wb_ram_addr(17'h0027F), 8'hEA);  // NOP
        mock_system.spi_write_at(common_pkg::wb_reg_addr(REG_BP_CTL), 8'h01);
        repeat (10) @(posedge cpu_clock);

        mock_system.spi_read_at(common_pkg::wb_reg_addr(REG_STATUS), dout);
        $display("[%t]   STATUS after final clear = %02x", $time, dout);
        `assert_equal(dout[REG_STATUS_BP_HALT_BIT], 1'b0);

        // ---- Stop CPU and restore initial state (ready=0, reset asserted) ----
        mock_system.spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0010);
        mock_system.cpu_stop;

        $display("[%t] End Breakpoint Test", $time);
    endtask

    task static open_bus_test;
        logic [DATA_WIDTH-1:0] dout;

        $display("[%t] Begin Open Bus Test", $time);

        // Seed the bus by writing a known value to mapped RAM.
        mock_system.cpu_write(16'h0400, 8'hA5);

        // Reading an unmapped address ($E800-$E80F) should return the last
        // byte that was on the CPU data bus.
        mock_system.cpu_read(16'hE800, dout);
        $display("[%t]   Open bus after $A5 write -> %02x", $time, dout);
        `assert_equal(dout, 8'hA5);

        // A different value should track.
        mock_system.cpu_write(16'h0401, 8'h42);
        mock_system.cpu_read(16'hE801, dout);
        $display("[%t]   Open bus after $42 write -> %02x", $time, dout);
        `assert_equal(dout, 8'h42);

        // Reading from a mapped address should return normally, not open bus.
        mock_system.cpu_read(16'h0400, dout);
        `assert_equal(dout, 8'hA5);

        // After reading mapped RAM ($A5), open bus should now reflect that.
        mock_system.cpu_read(16'hE80F, dout);
        $display("[%t]   Open bus after RAM read  -> %02x", $time, dout);
        `assert_equal(dout, 8'hA5);

        $display("[%t] End Open Bus Test", $time);
    endtask

    task static run;
        logic [DATA_WIDTH-1:0] cpu_dout;
        int count;


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
        open_bus_test;
        breakpoint_test;

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

    endtask

    `TB_INIT
endmodule
