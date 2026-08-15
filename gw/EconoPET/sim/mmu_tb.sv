// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

// Unit test for the SuperPET latches + Super-OS/9 MMU in address_decoding:
// bank-pair decode ($EFFC/$EFFD), control write-protect (bit 7), system
// latch RAM write-protect, flat mode entry, and SYNC exit with FIRQ pulse.
module mmu_tb;
    logic sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(sys_clock));
    initial clock_gen.start;

    logic reset = 0, be = 0, wr_strobe = 0, sync_st = 0;
    logic [15:0] addr = '0;
    logic [7:0] data = '0;
    logic ram_en, sid_en, pia1_en, pia2_en, via_en, crtc_en, io_en, unmapped, is_vram, is_ro;
    logic a12, a13, a14, a15, a16;
    logic flat, wp, firq_n;

    address_decoding dut (
        .reset_i(reset),
        .sys_clock_i(sys_clock),
        .cpu_be_i(be),
        .cpu_wr_strobe_i(wr_strobe),
        .cpu_addr_i(addr),
        .cpu_data_i(data),
        .superpet_en_i(1'b1),   // MMU/latches active (6809 selected)
        .ram_en_o(ram_en), .sid_en_o(sid_en), .pia1_en_o(pia1_en),
        .pia2_en_o(pia2_en), .via_en_o(via_en), .crtc_en_o(crtc_en),
        .io_en_o(io_en), .unmapped_o(unmapped), .is_vram_o(is_vram),
        .is_readonly_o(is_ro),
        .decoded_a12_o(a12), .decoded_a13_o(a13), .decoded_a14_o(a14),
        .decoded_a15_o(a15), .decoded_a16_o(a16),
        .sync_i(sync_st),
        .superpet_flat_o(flat),
        .superpet_wp_o(wp),
        .superpet_firq_n_o(firq_n)
    );

    task automatic cpu_write(input logic [15:0] a, input logic [7:0] v);
        @(posedge sys_clock);
        addr <= a; data <= v; be <= 1;
        repeat (2) @(posedge sys_clock);
        wr_strobe <= 1;
        @(posedge sys_clock);
        wr_strobe <= 0;
        @(posedge sys_clock);
        be <= 0;
        repeat (2) @(posedge sys_clock);
    endtask

    // Present an address (read decode settles while be)
    task automatic decode_at(input logic [15:0] a);
        @(posedge sys_clock);
        addr <= a; be <= 1;
        repeat (3) @(posedge sys_clock);
    endtask

    task static run;
        $display("[%t] BEGIN MMU test", $time);

        // --- Bank pair: STD $EFFC style (hi byte to $EFFC, bank to $EFFD) ---
        cpu_write(16'hEFFC, 8'h00);
        cpu_write(16'hEFFD, 8'h07);
        decode_at(16'h9123);
        `assert_equal({a15, a14, a13, a12}, 4'd7);   // bank 7 in a15..a12
        `assert_equal(a16, 1'b1);
        `assert_equal(ram_en, 1'b1);
        be <= 0; repeat (2) @(posedge sys_clock);

        // --- STB $EFFC form also banks (pair decode, VICE-faithful) ---
        cpu_write(16'hEFFC, 8'h03);
        decode_at(16'h9000);
        `assert_equal({a15, a14, a13, a12}, 4'd3);
        be <= 0; repeat (2) @(posedge sys_clock);

        // --- System latch is locked (ctrlwp): $EFF8 write must NOT take ---
        cpu_write(16'hEFF8, 8'h00);                  // try to engage RAM WP
        decode_at(16'h9000);
        `assert_equal(wp, 1'b0);                     // still writable
        be <= 0; repeat (2) @(posedge sys_clock);

        // --- Unlock (bit 7 high), engage RAM WP, verify, then release ---
        cpu_write(16'hEFFC, 8'h83);                  // unlock + keep bank 3
        cpu_write(16'hEFF8, 8'h00);                  // D1=0: write-protect
        decode_at(16'h9ABC);
        `assert_equal(wp, 1'b1);                     // banked window protected
        be <= 0; repeat (2) @(posedge sys_clock);
        decode_at(16'h1234);
        `assert_equal(wp, 1'b0);                     // main RAM unaffected
        be <= 0; repeat (2) @(posedge sys_clock);
        cpu_write(16'hEFF8, 8'h02);                  // D1=1: read/write again
        decode_at(16'h9ABC);
        `assert_equal(wp, 1'b0);
        be <= 0; repeat (2) @(posedge sys_clock);

        // --- Flat mode: everything is RAM in the upper 64K ---
        cpu_write(16'hEFFC, 8'h40);                  // bit6: flat
        `assert_equal(flat, 1'b1);
        decode_at(16'hC123);                         // normally ROM
        `assert_equal(ram_en, 1'b1);
        `assert_equal(is_ro, 1'b0);
        `assert_equal(a16, 1'b1);
        `assert_equal({a15, a14, a13, a12}, 4'hC);   // identity mapping
        be <= 0; repeat (2) @(posedge sys_clock);
        decode_at(16'hE823);                         // normally PIA2
        `assert_equal(pia2_en, 1'b0);
        `assert_equal(ram_en, 1'b1);
        be <= 0; repeat (2) @(posedge sys_clock);

        // --- SYNC exits flat mode: bank 0, re-locked, FIRQ pulse ---
        `assert_equal(firq_n, 1'b1);
        @(posedge sys_clock);
        sync_st <= 1;
        repeat (2) @(posedge sys_clock);
        sync_st <= 0;
        `assert_equal(flat, 1'b0);
        `assert_equal(firq_n, 1'b0);                 // FIRQ asserted (pulse)
        decode_at(16'h9000);
        `assert_equal({a15, a14, a13, a12}, 4'd0);   // back to bank 0
        be <= 0;
        // pulse must end on its own (~16us)
        repeat (1100) @(posedge sys_clock);
        `assert_equal(firq_n, 1'b1);

        // --- FIRQ-disable: SYNC exit without the wake pulse ---
        cpu_write(16'hEFFC, 8'h60);                  // flat + FIRQ disable
        `assert_equal(flat, 1'b1);
        @(posedge sys_clock);
        sync_st <= 1;
        repeat (2) @(posedge sys_clock);
        sync_st <= 0;
        `assert_equal(flat, 1'b0);
        `assert_equal(firq_n, 1'b1);                 // no pulse
        $display("[%t] END MMU test", $time);
    endtask

    `TB_INIT
endmodule
