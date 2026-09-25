// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

// Unit test for the SuperPET latches + Super-OS/9 MMU in address_decoding:
// bank-pair decode ($EFFC/$EFFD), control write-protect (bit 7), system
// latch RAM write-protect, flat mode, and the SYNC-triggered latch clear.
module mmu_tb;
    logic sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(sys_clock));
    initial clock_gen.start;

    logic reset = 0, be = 0, addr_strobe = 0, wr_strobe = 0, sync_st = 0;
    logic [15:0] addr = '0;
    logic [7:0] data = '0;
    logic ram_en, sid_en, pia1_en, pia2_en, via_en, crtc_en, io_en, unmapped, is_vram, is_ro;
    logic a12, a13, a14, a15, a16;
    logic flat, wp, firq_n;

    localparam logic [9:0] MAP_RAM      = 10'b1000000000;
    localparam logic [9:0] MAP_VRAM     = 10'b1000000010;
    localparam logic [9:0] MAP_SID      = 10'b0100000000;
    localparam logic [9:0] MAP_PIA1     = 10'b0010001000;
    localparam logic [9:0] MAP_PIA2     = 10'b0001001000;
    localparam logic [9:0] MAP_VIA      = 10'b0000101000;
    localparam logic [9:0] MAP_CRTC     = 10'b0000010000;
    localparam logic [9:0] MAP_UNMAPPED = 10'b0000000100;
    localparam logic [9:0] MAP_ROM      = 10'b1000000001;

    address_decoding dut (
        .reset_i(reset),
        .sys_clock_i(sys_clock),
        .cpu_be_i(be),
        .cpu_addr_strobe_i(addr_strobe),
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
        @(negedge sys_clock);
        addr = a;
        data = v;
        be = 1'b1;
        addr_strobe = 1'b1;
        @(posedge sys_clock);
        @(negedge sys_clock);
        addr_strobe = 1'b0;
        wr_strobe = 1'b1;
        @(posedge sys_clock);
        @(negedge sys_clock);
        wr_strobe = 1'b0;
        be = 1'b0;
        repeat (2) @(posedge sys_clock);
    endtask

    // Capture one CPU transaction at the address-valid strobe.
    task automatic decode_at(input logic [15:0] a);
        @(negedge sys_clock);
        addr = a;
        be = 1'b1;
        addr_strobe = 1'b1;
        @(posedge sys_clock);
        @(negedge sys_clock);
        addr_strobe = 1'b0;
    endtask

    task automatic check_decode(
        input logic [15:0] a,
        input logic [9:0] expected
    );
        decode_at(a);
        #1;
        `assert_equal(
            {ram_en, sid_en, pia1_en, pia2_en, via_en,
             crtc_en, io_en, unmapped, is_vram, is_ro},
            expected
        );
        be <= 0;
        repeat (2) @(posedge sys_clock);
    endtask

    task static check_banked_map;
        check_decode(16'h0000, MAP_RAM);
        check_decode(16'h8000, MAP_VRAM);
        check_decode(16'h8F00, MAP_SID);
        check_decode(16'h9000, MAP_RAM);
        check_decode(16'hA000, MAP_ROM);
        check_decode(16'hE800, MAP_UNMAPPED);
        check_decode(16'hE810, MAP_PIA1);
        check_decode(16'hE820, MAP_PIA2);
        check_decode(16'hE840, MAP_VIA);
        check_decode(16'hE880, MAP_CRTC);
        check_decode(16'hEFE0, MAP_UNMAPPED);
        check_decode(16'hEFF0, MAP_UNMAPPED);
        check_decode(16'hF000, MAP_ROM);
    endtask

    task static check_flat_map;
        be = 1'b1;
        addr_strobe = 1'b1;
        for (int unsigned a = 0; a < 65536; a++) begin
            @(negedge sys_clock);
            addr = 16'(a);
            @(posedge sys_clock);
            #1;
            if ({ram_en, sid_en, pia1_en, pia2_en, via_en,
                 crtc_en, io_en, unmapped, is_vram, is_ro} !== MAP_RAM)
                $fatal(1, "[%0t] flat decode mismatch at $%04x", $time, a);
            if ({a16, a15, a14, a13, a12} !== {1'b1, 4'(a >> 12)})
                $fatal(1, "[%0t] flat address mismatch at $%04x", $time, a);
            if (wp !== 1'b0)
                $fatal(1, "[%0t] flat write protection active at $%04x", $time, a);
        end
        addr_strobe = 1'b0;
        be = 1'b0;
        repeat (2) @(posedge sys_clock);
    endtask

    task static run;
        $display("[%t] BEGIN MMU test", $time);

        // --- No switch input: expansion RAM starts writable ---
        decode_at(16'h9000);
        `assert_equal(wp, 1'b0);
        be <= 0; repeat (2) @(posedge sys_clock);
        check_banked_map;

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

        // --- Unlock (bit 7 high), engage RAM WP, then release ---
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

        // --- CPU reset clears $EFFC but preserves the separate $EFF8 latch ---
        cpu_write(16'hEFFC, 8'h80);                  // unlock system latch
        cpu_write(16'hEFF8, 8'h00);                  // engage RAM WP
        reset <= 1;
        repeat (2) @(posedge sys_clock);
        reset <= 0;
        repeat (2) @(posedge sys_clock);
        decode_at(16'h9000);
        `assert_equal(wp, 1'b1);                     // RAM-WP state survived
        be <= 0; repeat (2) @(posedge sys_clock);
        cpu_write(16'hEFF8, 8'h02);                  // reset also re-locked it
        decode_at(16'h9000);
        `assert_equal(wp, 1'b1);
        be <= 0; repeat (2) @(posedge sys_clock);
        cpu_write(16'hEFFC, 8'h80);
        cpu_write(16'hEFF8, 8'h02);                  // restore writable state

        // Enable 8096 block 1 at $8000-$BFFF while retaining I/O peek-through.
        // Later, verify that a flat-mode $FFF0 write cannot alter this state.
        cpu_write(16'hFFF0, 8'hC4);
        decode_at(16'hA123);
        `assert_equal(a16, 1'b1);
        `assert_equal(a15, 1'b1);
        be <= 0; repeat (2) @(posedge sys_clock);

        // --- U3 changes immediately, while the active transaction is stable ---
        @(negedge sys_clock);
        addr = 16'hEFFC;
        data = 8'hC0;                                // flat + system latch unlocked
        be = 1'b1;
        addr_strobe = 1'b1;
        @(posedge sys_clock);
        #1;
        `assert_equal(flat, 1'b0);
        `assert_equal(unmapped, 1'b1);
        `assert_equal(ram_en, 1'b0);
        `assert_equal(a16, 1'b0);
        `assert_equal(a15, 1'b1);
        @(negedge sys_clock);
        addr_strobe = 1'b0;
        wr_strobe = 1'b1;
        @(posedge sys_clock);
        #1;
        `assert_equal(flat, 1'b1);
        `assert_equal(unmapped, 1'b1);
        `assert_equal(ram_en, 1'b0);
        `assert_equal(a16, 1'b0);
        `assert_equal(a15, 1'b1);
        @(negedge sys_clock);
        wr_strobe = 1'b0;
        @(posedge sys_clock);
        #1;
        `assert_equal(unmapped, 1'b1);
        `assert_equal(ram_en, 1'b0);
        `assert_equal(a16, 1'b0);
        `assert_equal(a15, 1'b1);
        @(negedge sys_clock);
        be = 1'b0;
        repeat (2) @(posedge sys_clock);

        // --- Flat mode: every logical address is upper expansion RAM ---
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
        check_flat_map;

        // In flat mode U1 forces the motherboard-facing address to $9xxx, so
        // logical I/O addresses access expansion RAM without affecting latches.
        // TEST.OS9 fills through $EFFC with $AA and depends on this isolation.
        cpu_write(16'hFFF0, 8'h80);                  // must not change $FFF0
        cpu_write(16'hEFF8, 8'h00);                  // system-latch aliases must
        cpu_write(16'hEFF9, 8'h00);                  // not engage RAM write
        cpu_write(16'hEFFA, 8'h00);                  // protection, even though
        cpu_write(16'hEFFB, 8'h00);                  // D7 was left unlocked
        cpu_write(16'hEFFC, 8'hAA);
        `assert_equal(flat, 1'b1);
        cpu_write(16'hEFFD, 8'h00);
        `assert_equal(flat, 1'b1);
        decode_at(16'hEFFC);
        `assert_equal(ram_en, 1'b1);
        `assert_equal(unmapped, 1'b0);
        be <= 0; repeat (2) @(posedge sys_clock);
        decode_at(16'hE823);
        `assert_equal(pia2_en, 1'b0);
        `assert_equal(ram_en, 1'b1);
        be <= 0; repeat (2) @(posedge sys_clock);

        // --- SYNC trigger is combinational; then it clears the $EFFC latch ---
        `assert_equal(firq_n, 1'b1);
        @(posedge sys_clock);
        sync_st <= 1;
        #1;
        `assert_equal(firq_n, 1'b0);
        @(posedge sys_clock);
        #1;
        `assert_equal(flat, 1'b0);
        decode_at(16'h9000);
        `assert_equal({a15, a14, a13, a12}, 4'd0);
        `assert_equal(wp, 1'b0);                     // flat $EFF8-$EFFB writes ignored
        be <= 0;
        sync_st <= 0;
        #1;
        `assert_equal(firq_n, 1'b1);
        decode_at(16'hA123);
        `assert_equal(a16, 1'b1);
        `assert_equal(a15, 1'b1);                    // flat $FFF0 write ignored
        be <= 0; repeat (2) @(posedge sys_clock);

        // Disable the preserved 8096 map before checking the normal SuperPET map.
        cpu_write(16'hFFF0, 8'h00);
        check_banked_map;

        // --- Banked SYNC clears every $EFFC bit and preserves $EFF8 ---
        cpu_write(16'hEFFC, 8'h87);                  // bank 7 + unlock system latch
        cpu_write(16'hEFF8, 8'h00);                  // engage RAM write protection
        sync_st <= 1;
        #1;
        `assert_equal(firq_n, 1'b0);
        @(posedge sys_clock);
        sync_st <= 0;
        decode_at(16'h9000);
        `assert_equal({a15, a14, a13, a12}, 4'd0);
        `assert_equal(wp, 1'b1);                     // separate $EFF8 latch survived
        be <= 0; repeat (2) @(posedge sys_clock);
        cpu_write(16'hEFF8, 8'h02);                  // SYNC re-locked system latch
        decode_at(16'h9000);
        `assert_equal(wp, 1'b1);
        be <= 0; repeat (2) @(posedge sys_clock);
        cpu_write(16'hEFFC, 8'h80);
        cpu_write(16'hEFF8, 8'h02);                  // restore writable state

        // --- SYNC-disable preserves bank, D7, and the separate $EFF8 latch ---
        cpu_write(16'hEFFC, 8'hA7);                  // bank 7 + disable + unlock
        cpu_write(16'hEFF8, 8'h00);                  // engage RAM write protection
        sync_st <= 1;
        #1;
        `assert_equal(firq_n, 1'b1);
        repeat (2) @(posedge sys_clock);
        sync_st <= 0;
        decode_at(16'h9000);
        `assert_equal({a15, a14, a13, a12}, 4'd7);
        `assert_equal(wp, 1'b1);
        be <= 0; repeat (2) @(posedge sys_clock);
        cpu_write(16'hEFF8, 8'h02);                  // D7 remained unlocked
        decode_at(16'h9000);
        `assert_equal(wp, 1'b0);
        be <= 0; repeat (2) @(posedge sys_clock);

        // D5 also prevents the flat-mode latch from being cleared.
        cpu_write(16'hEFFC, 8'h60);                  // flat + SYNC/FIRQ disable
        `assert_equal(flat, 1'b1);
        @(posedge sys_clock);
        sync_st <= 1;
        #1;
        `assert_equal(firq_n, 1'b1);
        repeat (2) @(posedge sys_clock);
        `assert_equal(flat, 1'b1);
        `assert_equal(firq_n, 1'b1);
        sync_st <= 0;
        $display("[%t] END MMU test", $time);
    endtask

    `TB_INIT
endmodule
