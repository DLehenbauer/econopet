// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Unit test for the IEEE-488 drive emulation (ieee.sv). The testbench plays
// both external roles:
//
//  - the CPU/controller, issuing the exact register-level sequence the
//    Waterloo kernel uses (LISTEN/OPEN/name/UNLISTEN, then
//    TALK + status/data reads with full NRFD/NDAC/DAV handshakes), and
//  - the MCU, servicing the FIFOs over the Wishbone peripheral port.
module ieee_tb;
    bit sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) sys_clock_gen (.clock_o(sys_clock));
    initial sys_clock_gen.start;

    // Wishbone (MCU side)
    logic [WB_ADDR_WIDTH-1:0] wb_addr = '0;
    logic [DATA_WIDTH-1:0] wb_dout = '0;
    logic [DATA_WIDTH-1:0] wb_din;
    logic wb_we = 0, wb_cycle = 0, wb_strobe = 0;
    logic wb_stall, wb_ack;

    // Inter-bus-cycle gap (sys-clocks appended after each CPU bus op). The
    // real arbiter keeps the intra-cycle addr->data timing fixed; only the
    // gap between bus cycles varies. Lower g_tail packs consecutive
    // $E820-read / NDAC-re-arm ops denser -- the worst case for the talker's
    // consumption-gated pop. Default 2 keeps the existing tests bit-identical.
    int g_tail = 2;

    // CPU bus (controller side)
    logic cpu_be = 0, cpu_addr_strobe = 0, cpu_data_strobe = 0, cpu_we = 0;
    logic [DATA_WIDTH-1:0] cpu_data = '0;
    logic [DATA_WIDTH-1:0] dut_data;
    logic dut_doe;
    logic pia1_cs = 0, pia2_cs = 0, via_cs = 0;
    logic [VIA_RS_WIDTH-1:0] rs = '0;

    ieee dut (
        .wb_clock_i(sys_clock),
        .wbp_addr_i(wb_addr),
        .wbp_data_i(wb_dout),
        .wbp_data_o(wb_din),
        .wbp_we_i(wb_we),
        .wbp_cycle_i(wb_cycle),
        .wbp_strobe_i(wb_strobe),
        .wbp_stall_o(wb_stall),
        .wbp_ack_o(wb_ack),
        .wbp_sel_i(wb_cycle),          // selected for every cycle

        .cpu_be_i(cpu_be),
        .cpu_addr_strobe_i(cpu_addr_strobe),
        .cpu_data_strobe_i(cpu_data_strobe),
        .cpu_data_i(cpu_data),
        .cpu_data_o(dut_data),
        .cpu_data_oe(dut_doe),
        .cpu_we_i(cpu_we),

        .pia1_cs_i(pia1_cs),
        .pia2_cs_i(pia2_cs),
        .via_cs_i(via_cs),
        .rs_i(rs),

        .diag_i(1'b1),
        .vert_i(1'b1)
    );

    // ------------------------------------------------------------------
    // MCU-side helpers
    // ------------------------------------------------------------------
    task automatic mcu_write(input logic [IEEE_REG_ADDR_WIDTH-1:0] r, input logic [7:0] v);
        @(posedge sys_clock);
        wb_addr   = common_pkg::wb_ieee_addr(r);
        wb_dout   = v;
        wb_we     = 1;
        wb_cycle  = 1;
        wb_strobe = 1;
        @(posedge sys_clock);
        wb_strobe = 0;
        wb_cycle  = 0;
        wb_we     = 0;
        @(posedge sys_clock);
    endtask

    task automatic mcu_read(input logic [IEEE_REG_ADDR_WIDTH-1:0] r, output logic [7:0] v);
        @(posedge sys_clock);
        wb_addr   = common_pkg::wb_ieee_addr(r);
        wb_we     = 0;
        wb_cycle  = 1;
        wb_strobe = 1;
        @(posedge sys_clock);
        wb_strobe = 0;
        wb_cycle  = 0;
        @(posedge sys_clock);
        v = wb_din;
    endtask

    // ------------------------------------------------------------------
    // CPU-side helpers (register-level bus cycles)
    // ------------------------------------------------------------------
    task automatic cpu_write(input logic p1, input logic p2, input logic v,
                             input logic [3:0] r, input logic [7:0] val);
        // All bus signals driven with NBA (<=): the DUT's FFs sample on the
        // same posedges this task wakes on, so blocking drives race the
        // sampling (a 1-clock strobe can be cleared before some FFs see it).
        // On real hardware these are FF outputs -- NBA models that.
        @(posedge sys_clock);
        rs <= r; cpu_data <= val; cpu_we <= 1; cpu_be <= 1;
        cpu_addr_strobe <= 1;
        @(posedge sys_clock);
        cpu_addr_strobe <= 0;
        // Chip-selects settle after the strobe on real hardware (the decode
        // is registered off an address that is itself a clock behind the
        // strobe) -- model that so a too-early snapshot in the DUT fails.
        pia1_cs <= p1; pia2_cs <= p2; via_cs <= v;
        repeat (4) @(posedge sys_clock);
        cpu_data_strobe <= 1;
        @(posedge sys_clock);
        cpu_data_strobe <= 0;
        @(posedge sys_clock);
        cpu_be <= 0; cpu_we <= 0; pia1_cs <= 0; pia2_cs <= 0; via_cs <= 0;
        repeat (g_tail) @(posedge sys_clock);
    endtask

    // Read returns injected value when the DUT drives, else 8'hFF (open bus).
    task automatic cpu_read(input logic p1, input logic p2, input logic v,
                            input logic [3:0] r, output logic [7:0] val);
        @(posedge sys_clock);
        rs <= r; cpu_we <= 0; cpu_be <= 1;
        cpu_addr_strobe <= 1;
        @(posedge sys_clock);
        cpu_addr_strobe <= 0;
        pia1_cs <= p1; pia2_cs <= p2; via_cs <= v;   // CS settles after strobe
        repeat (5) @(posedge sys_clock);
        val = dut_doe ? dut_data : 8'hFF;
        cpu_data_strobe <= 1;      // the CPU's read of this register
        @(posedge sys_clock);
        cpu_data_strobe <= 0;
        @(posedge sys_clock);
        cpu_be <= 0; pia1_cs <= 0; pia2_cs <= 0; via_cs <= 0;
        repeat (g_tail) @(posedge sys_clock);
    endtask

    // Controller-side IEEE line manipulation (mirrors kernel usage)
    logic [7:0] orb_shadow = 8'hFF;

    task automatic ctl_atn(input logic asserted);
        orb_shadow[2] = !asserted;
        cpu_write(0, 0, 1, 4'd0, orb_shadow);
    endtask

    task automatic ctl_nrfd(input logic asserted);
        orb_shadow[1] = !asserted;
        cpu_write(0, 0, 1, 4'd0, orb_shadow);
    endtask

    task automatic ctl_dav(input logic asserted);
        cpu_write(0, 1, 0, 4'd3, asserted ? 8'h34 : 8'h3C);  // CRB: CB2 level
    endtask

    task automatic ctl_ndac(input logic asserted);
        cpu_write(0, 1, 0, 4'd1, asserted ? 8'h34 : 8'h3C);  // CRA: CA2 level
    endtask

    task automatic ctl_dio(input logic [7:0] val);
        cpu_write(0, 1, 0, 4'd2, ~val);
    endtask

    task automatic ctl_read_status(output logic [7:0] pb);
        cpu_read(0, 0, 1, 4'd0, pb);
    endtask

    // Poll $E840 until (value & mask) == want, with a cycle budget.
    task automatic ctl_wait(input logic [7:0] mask, input logic [7:0] want);
        logic [7:0] pb;
        int guard;
        guard = 0;
        pb = 8'h00;
        ctl_read_status(pb);
        while ((pb & mask) != want) begin
            guard++;
            if (guard > 200) $fatal(1, "ctl_wait timeout (mask=%02x want=%02x last=%02x)", mask, want, pb);
            ctl_read_status(pb);
        end
    endtask

    // Send one byte as controller/talker (used for ATN commands and listener data).
    task automatic ctl_send(input logic [7:0] b);
        ctl_ndac(0);                   // release own acceptor lines first
        ctl_nrfd(0);
        ctl_dio(b);
        ctl_wait(8'h01, 8'h00);        // a device holds NDAC asserted
        ctl_dav(1);
        ctl_wait(8'h01, 8'h01);        // NDAC released = accepted
        ctl_dav(0);
        ctl_wait(8'h01, 8'h00);        // device re-arms NDAC
        cpu_write(0, 1, 0, 4'd2, 8'hFF);   // release DIO (raw port value)
    endtask

    // Receive one byte as controller/acceptor (device is talker).
    task automatic ctl_recv(output logic [7:0] b, output logic eoi);
        logic [7:0] pa;
        ctl_ndac(1);
        ctl_nrfd(0);
        ctl_wait(8'h80, 8'h00);        // DAV asserted: data valid
        cpu_read(0, 1, 0, 4'd0, b);    // $E820 DIO in (active low)
        b = ~b;
        cpu_read(1, 0, 0, 4'd0, pa);   // $E810: EOI on bit 6
        eoi = !pa[6];
        ctl_nrfd(1);
        ctl_ndac(0);
        ctl_wait(8'h80, 8'h80);        // DAV released
        ctl_ndac(1);
        ctl_nrfd(0);
    endtask

    task automatic mcu_push(input string s, input bit final_cr);
        for (int i = 0; i < s.len(); i++) mcu_write(3'd6, s[i]);          // TXS
        if (final_cr) mcu_write(3'd7, 8'h0D);                             // TXS_LAST
    endtask

    // Drain the RX FIFO, printing tagged bytes; returns them in a buffer.
    byte rx_bytes [$];
    bit  rx_isatn [$];

    task automatic mcu_drain_rx;
        logic [7:0] st, d;
        mcu_read(IEEE_REG_STATUS, st);
        while (st[0]) begin
            rx_isatn.push_back(st[1]);
            mcu_read(IEEE_REG_RX, d);
            mcu_write(IEEE_REG_RX, 8'h00);   // explicit pop
            rx_bytes.push_back(d);
            mcu_read(IEEE_REG_STATUS, st);
        end
    endtask

    task static run;
        logic [7:0] d, st;
        logic eoi;
        string name;

        $display("[%t] BEGIN IEEE drive emulation test", $time);

        // MCU: enable + flush
        mcu_write(IEEE_REG_CTRL, 8'h03);
        mcu_write(IEEE_REG_CTRL, 8'h01);

        // Kernel-style init of controller-side registers
        cpu_write(0, 1, 0, 4'd1, 8'h3C);   // PIA2 CRA: CA2 high (NDAC released)
        cpu_write(0, 1, 0, 4'd3, 8'h3C);   // PIA2 CRB: CB2 high (DAV released)
        cpu_write(0, 1, 0, 4'd2, 8'hFF);   // DIO released
        cpu_write(0, 0, 1, 4'd0, 8'hFF);   // VIA ORB: ATN/NRFD released

        // --- Command phase: LISTEN 8, OPEN ch0, name, UNLISTEN ---
        ctl_atn(1);
        ctl_send(8'h28);                   // LISTEN 8
        ctl_send(8'hF0);                   // OPEN ch0
        ctl_atn(0);
        name = "1:BASIC,PRG";
        for (int i = 0; i < name.len(); i++) ctl_send(name[i]);
        ctl_atn(1);
        ctl_send(8'h3F);                   // UNLISTEN
        ctl_atn(0);

        // MCU sees the tagged command/data stream
        mcu_drain_rx;
        `assert_equal(rx_bytes.size(), 14);
        `assert_equal(rx_isatn[0], 1);  `assert_equal(rx_bytes[0], 8'h28);
        `assert_equal(rx_isatn[1], 1);  `assert_equal(rx_bytes[1], 8'hF0);
        `assert_equal(rx_isatn[2], 0);  `assert_equal(rx_bytes[2], "1");
        `assert_equal(rx_isatn[12], 0); `assert_equal(rx_bytes[12], "G");
        `assert_equal(rx_isatn[13], 1); `assert_equal(rx_bytes[13], 8'h3F);
        mcu_read(IEEE_REG_SA, d);
        `assert_equal(d, 8'hF0);

        // --- kernel TALKs channel 15; MCU pushes status after seeing $6F ---
        ctl_atn(1);
        ctl_send(8'h48);                   // TALK 8
        ctl_send(8'h6F);                   // secondary 15
        ctl_atn(0);
        mcu_push("00, OK,00,00", 1);
        // controller becomes acceptor
        begin
            string status_str = "";
            eoi = 0;
            while (!eoi) begin
                ctl_recv(d, eoi);
                if (!eoi) status_str = {status_str, string'(d)};
            end
            `assert_equal(d, 8'h0D);
            $display("[%t]   status read: '%s' + CR(EOI)", $time, status_str);
            assert(status_str == "00, OK,00,00") else $fatal(1, "bad status '%s'", status_str);
        end
        ctl_atn(1);
        ctl_send(8'h5F);                   // UNTALK
        ctl_atn(0);

        // --- MCU queues file data; kernel TALKs channel 0 ---
        mcu_drain_rx;                      // consume TALK/UNTALK commands
        for (int i = 0; i < 32; i++) begin
            if (i == 31) mcu_write(IEEE_REG_TX_LAST, 8'h40 + i[7:0]);
            else         mcu_write(IEEE_REG_TX, 8'h40 + i[7:0]);
        end

        ctl_atn(1);
        ctl_send(8'h48);                   // TALK 8
        ctl_send(8'h60);                   // secondary 0
        ctl_atn(0);
        // First pass: read only 10 bytes (like the kernel's header read)...
        for (int i = 0; i < 10; i++) begin
            ctl_recv(d, eoi);
            `assert_equal(d, 8'h40 + i[7:0]);
        end
        // ...then terminate exactly like the Waterloo kernel (C150): the
        // per-byte loop has already re-armed NDAC, so this fast talker has
        // byte 10's DAV asserted. The kernel now releases NRFD and NDAC
        // without reading DIO -- line-identical to an accept -- waits for
        // DAV to clear, and only then asserts ATN. The DUT must NOT pop
        // byte 10 (pop-on-consumption); the resume loop below starts at 10
        // and is off-by-one if the phantom accept is not suppressed.
        ctl_ndac(0);                       // release without a data read
        ctl_wait(8'h80, 8'h80);            // talker backs off (DAV released)
        ctl_atn(1);
        ctl_send(8'h5F);                   // UNTALK mid-file
        ctl_send(8'h48);
        ctl_send(8'h6F);                   // status again
        ctl_atn(0);
        mcu_push("00, OK,00,00", 1);
        eoi = 0;
        while (!eoi) ctl_recv(d, eoi);
        `assert_equal(d, 8'h0D);
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_send(8'h48);
        ctl_send(8'h60);                   // resume data channel
        ctl_atn(0);
        for (int i = 10; i < 32; i++) begin
            ctl_recv(d, eoi);
            `assert_equal(d, 8'h40 + i[7:0]);
            if (i == 31) begin
                `assert_equal(eoi, 1'b1);
            end else begin
                `assert_equal(eoi, 1'b0);
            end
        end
        $display("[%t]   continuation across UNTALK verified", $time);
        ctl_atn(1);
        ctl_send(8'h5F);                   // UNTALK
        ctl_send(8'h28);                   // LISTEN 8
        ctl_send(8'hE0);                   // CLOSE ch0
        ctl_send(8'h3F);                   // UNLISTEN
        ctl_atn(0);

        mcu_drain_rx;
        mcu_read(IEEE_REG_SA, d);
        `assert_equal(d, 8'hE0);

        // --- RX-full backpressure: the DAV edge must survive a full FIFO ---
        // Fill RX to the brim (2 command bytes + 30 data = 32), then send one
        // more data byte with no room; the device must hold NDAC (not ACK,
        // not drop) until the MCU drains, then complete the handshake.
        rx_bytes.delete(); rx_isatn.delete();
        ctl_atn(1);
        ctl_send(8'h28);                   // LISTEN 8
        ctl_send(8'hF1);                   // OPEN ch1
        ctl_atn(0);
        for (int i = 0; i < 30; i++) ctl_send(8'h20 + i[7:0]);
        fork
            ctl_send(8'hA5);               // stalls at the full FIFO...
            begin
                #5us;                      // ...until the MCU drains it
                mcu_drain_rx;
            end
        join
        mcu_drain_rx;                      // collect the A5 pushed after drain
        `assert_equal(rx_bytes.size(), 33);
        `assert_equal(rx_bytes[32], 8'hA5);
        `assert_equal(rx_bytes[31], 8'h3D); // 0x20+29
        ctl_atn(1);
        ctl_send(8'h3F);                   // UNLISTEN
        ctl_atn(0);
        mcu_drain_rx;
        $display("[%t]   RX-full backpressure verified", $time);

        // --- ATN abort after consumption must pop (no duplicate byte) ---
        for (int i = 0; i < 4; i++) mcu_write(IEEE_REG_TX, 8'h60 + i[7:0]);
        mcu_write(IEEE_REG_TX_LAST, 8'h64);
        ctl_atn(1);
        ctl_send(8'h48);                   // TALK 8
        ctl_send(8'h61);                   // secondary 1
        ctl_atn(0);
        ctl_ndac(1);
        ctl_nrfd(0);
        ctl_wait(8'h80, 8'h00);            // DAV asserted: byte 0 in flight
        cpu_read(0, 1, 0, 4'd0, d);        // CPU reads $E820 (consumes)...
        `assert_equal(~d, 8'h60);
        ctl_atn(1);                        // ...but asserts ATN before NDAC release
        ctl_send(8'h5F);                   // UNTALK
        ctl_send(8'h48);
        ctl_send(8'h61);                   // TALK again
        ctl_atn(0);
        for (int i = 1; i < 5; i++) begin  // stream must resume at byte 1
            ctl_recv(d, eoi);
            `assert_equal(d, 8'h60 + i[7:0]);
        end
        `assert_equal(eoi, 1'b1);
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
        $display("[%t]   ATN-after-consume pop verified", $time);

        // --- dense counted-read + terminate + resume (the Super-OS/9 REL
        // per-record pattern) at a tight 1MHz cadence ---
        // The kernel reads a counted number of bytes then terminates mid-stream
        // (release NDAC without reading the next byte's DIO -- the phantom
        // accept), UNTALKs, and re-TALKs to continue. At the packed cadence the
        // fast talker already has the next byte's DAV up during terminate; the
        // byte_consumed guard must NOT pop it, or the resume is off-by-one.
        // Several records: the race is timing-dependent.
        begin
            int saved_tail = g_tail;
            for (int rec = 0; rec < 6; rec++) begin
                int cnt = 8 + rec;            // vary the counted length per record
                mcu_drain_rx;
                for (int i = 0; i < 24; i++) mcu_write(IEEE_REG_TX, 8'h80 + i[7:0]);
                mcu_write(IEEE_REG_TX_LAST, 8'h98);   // 25 bytes (0x80..0x98)
                ctl_atn(1); ctl_send(8'h48); ctl_send(8'h60); ctl_atn(0);
                g_tail = 0;                  // tight per-byte cadence
                // Counted read of 'cnt' bytes...
                for (int i = 0; i < cnt; i++) begin
                    ctl_recv(d, eoi);
                    `assert_equal(d, 8'h80 + i[7:0]);
                end
                // ...terminate mid-record exactly like the kernel (no DIO read).
                ctl_ndac(0);
                ctl_wait(8'h80, 8'h80);      // talker backs off (DAV released)
                ctl_atn(1); ctl_send(8'h5F); ctl_send(8'h48); ctl_send(8'h60); ctl_atn(0);
                // Resume: MUST continue at byte 'cnt' (off-by-one if dup/drop).
                for (int i = cnt; i < 25; i++) begin
                    ctl_recv(d, eoi);
                    `assert_equal(d, 8'h80 + i[7:0]);
                end
                g_tail = saved_tail;
                ctl_atn(1); ctl_send(8'h5F); ctl_atn(0);
                mcu_drain_rx;
                mcu_write(IEEE_REG_CTRL, 8'h05);   // flush data FIFO between records
            end
            $display("[%t]   dense counted-read terminate/resume verified", $time);
        end

        // --- Unit 9: the fabric answers DEV_ADDR+1 with the same machinery
        // (Super-OS/9 d8d9 = dual units 8 and 9). LISTEN 9 must capture data
        // into RX (tagged commands show $29), TALK 9 must serve the TX FIFO. ---
        ctl_atn(1);
        ctl_send(8'h29);                   // LISTEN 9
        ctl_send(8'h62);                   // secondary 2
        ctl_atn(0);
        ctl_send(8'h55);                   // one data byte
        ctl_atn(1);
        ctl_send(8'h3F);                   // UNLISTEN
        ctl_atn(0);
        begin
            int base;
            base = rx_bytes.size();
            mcu_drain_rx;
            `assert_equal(rx_bytes.size(), base + 4);
            `assert_equal(rx_isatn[base + 0], 1); `assert_equal(rx_bytes[base + 0], 8'h29);
            `assert_equal(rx_isatn[base + 1], 1); `assert_equal(rx_bytes[base + 1], 8'h62);
            `assert_equal(rx_isatn[base + 2], 0); `assert_equal(rx_bytes[base + 2], 8'h55);
            `assert_equal(rx_isatn[base + 3], 1); `assert_equal(rx_bytes[base + 3], 8'h3F);
        end
        for (int i = 0; i < 3; i++) mcu_write(IEEE_REG_TX, 8'h70 + i[7:0]);
        mcu_write(IEEE_REG_TX_LAST, 8'h73);
        ctl_atn(1);
        ctl_send(8'h49);                   // TALK 9
        ctl_send(8'h62);
        ctl_atn(0);
        for (int i = 0; i < 4; i++) begin
            ctl_recv(d, eoi);
            `assert_equal(d, 8'h70 + i[7:0]);
        end
        `assert_equal(eoi, 1'b1);
        ctl_atn(1);
        ctl_send(8'h5F);                   // UNTALK
        ctl_atn(0);
        mcu_drain_rx;
        $display("[%t]   unit 9 (DEV_ADDR+1) listen/talk verified", $time);

        // Idle: device releases everything
        mcu_read(IEEE_REG_STATUS, st);
        `assert_equal(st[6], 0);           // not talking
        `assert_equal(st[5], 0);           // not listening

        $display("[%t] END IEEE drive emulation test", $time);
    endtask

    `TB_INIT
endmodule
