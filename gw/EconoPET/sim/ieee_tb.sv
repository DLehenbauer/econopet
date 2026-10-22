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
    logic ifc = 1'b0;

    // Wishbone (MCU side)
    logic [WB_ADDR_WIDTH-1:0] wb_addr = '0;
    logic [DATA_WIDTH-1:0] wb_dout = '0;
    logic [DATA_WIDTH-1:0] wb_din;
    logic wb_we = 0, wb_cycle = 0, wb_strobe = 0;
    logic wb_stall, wb_ack;

    // Inter-bus-cycle gap (sys-clocks appended after each CPU bus op). The
    // real arbiter keeps the intra-cycle addr->data timing fixed; only the
    // gap between bus cycles varies. Lower g_tail packs consecutive
    // NRFD / NDAC-re-arm ops denser, exercising prefetch and termination
    // at the tightest cadence. Default 2 preserves the ordinary spacing.
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
        .ifc_i(ifc),
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

    // Set the PET's PIA1 CA2 output independently of ATN and DAV.
    task automatic ctl_eoi(input logic asserted);
        cpu_write(1, 0, 0, 4'd1, asserted ? 8'h34 : 8'h3C);
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
            if (guard > 200)
                $fatal(1, "ctl_wait timeout (mask=%02x want=%02x last=%02x talk=%0d sa=%02x rx=%0d atn_end=%b event=%b)",
                    mask, want, pb, dut.talk_st, dut.talk_sa, dut.rx_count,
                    dut.atn_end_pending, dut.talk_event_pending);
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
        ctl_nrfd(1);                   // native input asserts busy before sampling
        cpu_read(0, 1, 0, 4'd0, b);    // $E820 DIO in (active low)
        b = ~b;
        cpu_read(1, 0, 0, 4'd0, pa);   // $E810: EOI on bit 6
        eoi = !pa[6];
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
    bit  rx_iseoi [$];
    byte rx_talk_events [$];
    byte rx_metadata_tags [$];
    int rx_boundary_count = 0;

    task automatic mcu_drain_rx;
        logic [7:0] st, d, ctrl, repeated_ctrl;
        mcu_read(IEEE_REG_STATUS, st);
        while (st[0]) begin
            mcu_read(IEEE_REG_CTRL, ctrl);
            mcu_read(IEEE_REG_CTRL, repeated_ctrl);
            `assert_equal((ctrl & (IEEE_CTRL_RD_RX_EOI_MASK | IEEE_CTRL_RD_RX_ATN_END_MASK
                    | IEEE_CTRL_RD_RX_TALK_EVENT_MASK)),
                (repeated_ctrl & (IEEE_CTRL_RD_RX_EOI_MASK | IEEE_CTRL_RD_RX_ATN_END_MASK
                    | IEEE_CTRL_RD_RX_TALK_EVENT_MASK)));
            mcu_read(IEEE_REG_RX, d);
            mcu_write(IEEE_REG_RX, 8'h00);   // explicit pop
            if (ctrl & IEEE_CTRL_RD_RX_ATN_END_MASK) begin
                rx_boundary_count++;
                rx_metadata_tags.push_back(IEEE_CTRL_RD_RX_ATN_END_MASK);
            end else if (ctrl & IEEE_CTRL_RD_RX_TALK_EVENT_MASK) begin
                rx_talk_events.push_back(d);
                rx_metadata_tags.push_back(IEEE_CTRL_RD_RX_TALK_EVENT_MASK);
            end else begin
                rx_isatn.push_back(st[1]);
                rx_iseoi.push_back((ctrl & IEEE_CTRL_RD_RX_EOI_MASK) != 0);
                rx_bytes.push_back(d);
            end
            mcu_read(IEEE_REG_STATUS, st);
        end
    endtask

    // Source completion is ordered metadata, never an invented GET or bus byte.
    task automatic check_talk_event(input bit status_fifo, input bit fetched, input bit interrupted);
        logic [7:0] d;
        int first_event;
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        ctl_atn(1);
        ctl_send(8'h4B);
        ctl_send(status_fifo ? 8'h6F : 8'h63);
        ctl_nrfd(1);
        ctl_ndac(1);
        ctl_atn(0);
        mcu_write(status_fifo ? IEEE_REG_TXS_LAST : IEEE_REG_TX_LAST, "X");
        mcu_write(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, "Y");
        mcu_drain_rx;
        first_event = rx_talk_events.size();
        ctl_nrfd(0);
        ctl_wait(8'h80, 8'h00);
        if (fetched) ctl_nrfd(1);
        if (interrupted) begin
            // ATN can interrupt TLK35 after EOF GET but before NDAC releases.
            ctl_atn(1);
            ctl_send(8'h5F);
            ctl_atn(0);
        end else begin
            ctl_ndac(0);
            ctl_wait(8'h80, 8'h80);
            ctl_ndac(1);
            repeat (20) @(posedge sys_clock);
            `assert_equal(dut.dev_dav_n, 1'b1);
            if (!fetched) `assert_equal(dut.talk_st, dut.T_DONE);
        end
        mcu_drain_rx;
        if (status_fifo && fetched) begin
            `assert_equal(rx_talk_events.size(), first_event);
        end else begin
            `assert_equal(rx_talk_events.size(), first_event + 1);
            `assert_equal(rx_talk_events[first_event],
                (status_fifo ? 8'h3F : 8'h33)
                    | (fetched ? (interrupted ? 8'h80 : 8'hC0) : 8'h00));
        end
        mcu_read(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, d);
        `assert_equal(d, fetched ? 8'd1 : 8'd0);
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    // Check native prefetch independently of private reads for either TX FIFO.
    task automatic check_talk_abort(input bit status_fifo, input bit prefetch,
                                    input bit sample_data);
        localparam int PREFETCH_DWELL_CYCLES = 20;
        logic [7:0] d;
        logic eoi;

        mcu_drain_rx;
        mcu_write(IEEE_REG_CTRL, 8'h03);
        ctl_atn(1);
        ctl_send(8'h48);
        ctl_send(status_fifo ? 8'h6F : 8'h61);
        ctl_nrfd(1);
        ctl_ndac(1);
        ctl_atn(0);
        mcu_write(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, "X");
        mcu_write(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, "Y");
        mcu_write(status_fifo ? IEEE_REG_TXS_LAST : IEEE_REG_TX_LAST, "Z");

        // Hold DAV with NDAC asserted, optionally triggering the native GET.
        ctl_nrfd(0);
        ctl_wait(8'h80, 8'h00);
        if (prefetch) ctl_nrfd(1);
        if (sample_data) begin
            cpu_read(0, 1, 0, 4'd0, d);
            `assert_equal(~d, 8'h58);
        end
        repeat (PREFETCH_DWELL_CYCLES) @(posedge sys_clock);
        `assert_equal(dut.dev_dav_n, 1'b0);
        `assert_equal(dut.dev_dio, ~8'h58);
        `assert_equal(dut.dev_eoi_n, 1'b1);
        mcu_read(IEEE_REG_CTRL, d);
        `assert_equal((d & IEEE_CTRL_RD_DATA_ACTIVE_MASK),
            (status_fifo ? 8'd0 : IEEE_CTRL_RD_DATA_ACTIVE_MASK));
        `assert_equal(dut.cpu_ndac_n, 1'b0);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, (!status_fifo && prefetch) ? 8'd1 : 8'd0);
        mcu_read(IEEE_REG_TXS, d);
        `assert_equal(d, (status_fifo && prefetch) ? 8'd1 : 8'd0);
        mcu_read(IEEE_REG_TXS, d);
        `assert_equal(d, (status_fifo && prefetch) ? 8'd1 : 8'd0);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, (!status_fifo && prefetch) ? 8'd1 : 8'd0);

        // Readdress data only: a new status secondary would deliberately flush TXS.
        ctl_atn(1);
        if (!status_fifo) begin
            ctl_send(8'h5F);
            ctl_send(8'h48);
            ctl_send(8'h61);
        end
        ctl_nrfd(1);
        ctl_ndac(1);
        ctl_atn(0);
        if (!prefetch) begin
            ctl_recv(d, eoi);
            `assert_equal(d, "X");
            `assert_equal(eoi, 1'b0);
        end
        ctl_recv(d, eoi);
        `assert_equal(d, "Y");
        `assert_equal(eoi, 1'b0);
        ctl_recv(d, eoi);
        `assert_equal(d, "Z");
        `assert_equal(eoi, 1'b1);
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    // A peer acceptor must receive the selected source's bytes without UNLISTEN.
    task automatic check_peer_transfer(input bit cpu_receiver);
        logic [7:0] d;
        logic eoi;
        int first_data;
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        mcu_drain_rx;
        ctl_atn(1);
        ctl_send(8'h29);
        ctl_send(8'h62);
        ctl_send(8'h27);
        ctl_send(8'h48);
        ctl_send(8'h62);
        ctl_nrfd(1);
        ctl_ndac(cpu_receiver);
        ctl_atn(0);
        mcu_drain_rx;
        first_data = rx_bytes.size();
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | 8'h02);
        mcu_write(IEEE_REG_TX, 8'hA5);
        mcu_write(IEEE_REG_TX, 8'h00);
        mcu_write(IEEE_REG_TX_LAST, 8'h5A);
        if (cpu_receiver) begin
            ctl_recv(d, eoi);
            `assert_equal(d, 8'hA5);
            `assert_equal(eoi, 1'b0);
            ctl_recv(d, eoi);
            `assert_equal(d, 8'h00);
            `assert_equal(eoi, 1'b0);
            ctl_recv(d, eoi);
            `assert_equal(d, 8'h5A);
            `assert_equal(eoi, 1'b1);
        end else begin
            ctl_nrfd(0);
            repeat (100) @(posedge sys_clock);
        end
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd3);
        mcu_drain_rx;
        `assert_equal(rx_bytes.size(), first_data + 3);
        `assert_equal(rx_bytes[first_data], 8'hA5);
        `assert_equal(rx_bytes[first_data + 1], 8'h00);
        `assert_equal(rx_bytes[first_data + 2], 8'h5A);
        for (int i = 0; i < 3; ++i) begin
            `assert_equal(rx_isatn[first_data + i], 1'b0);
            `assert_equal(rx_iseoi[first_data + i], (i == 2));
        end
        `assert_equal(dut.listen_units, 4'b0010);
        ctl_atn(1);
        ctl_send(8'h3F);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    // Delayed MCU service must stop before a peer's full RX queue holds active DAV.
    task automatic check_peer_backpressure(input bit pending_boundary, input bit eof_at_capacity);
        localparam int RECORD_LENGTH = 40;
        int record_length;
        localparam int SELECTION_BYTES = 5;
        localparam int STALL_CYCLES = 400;
        logic [7:0] d;
        int first_data;
        int previous_boundaries;
        int previous_events;
        record_length = eof_at_capacity ? dut.RX_DEPTH : RECORD_LENGTH;
        $display("[%t]   peer backpressure boundary=%b eof_at_capacity=%b",
            $time, pending_boundary, eof_at_capacity);
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        mcu_drain_rx;
        ctl_atn(1);
        ctl_send(8'h29);
        ctl_send(8'h62);
        ctl_send(8'h27);
        ctl_send(8'h48);
        ctl_send(8'h62);
        if (pending_boundary)
            for (int i = SELECTION_BYTES; i < dut.RX_DEPTH - 1; ++i) ctl_send(8'h62);
        for (int i = 0; i < record_length; ++i)
            mcu_write(i == record_length - 1 ? IEEE_REG_TX_LAST : IEEE_REG_TX, 8'(i + 1));
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | 8'h02);
        ctl_ndac(0);
        ctl_nrfd(!pending_boundary);
        ctl_atn(0);
        if (pending_boundary) begin
            repeat (STALL_CYCLES) @(posedge sys_clock);
            `assert_equal(dut.rx_count, dut.RX_DEPTH);
            `assert_equal(dut.dev_dav_n, 1'b1);
            mcu_read(IEEE_REG_TX, d);
            `assert_equal(d, 8'd0);
            mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | IEEE_TX_OWNER_PAUSE_MASK | 8'h02);
        end
        // Drain selection metadata before allowing the bounded data burst.
        previous_boundaries = rx_boundary_count;
        mcu_drain_rx;
        `assert_equal(rx_boundary_count, previous_boundaries + 1);
        first_data = rx_bytes.size();
        previous_events = rx_talk_events.size();
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | 8'h02);
        ctl_nrfd(0);
        repeat (STALL_CYCLES) @(posedge sys_clock);
        `assert_equal(dut.rx_count, dut.RX_DEPTH);
        `assert_equal(dut.dev_dav_n, 1'b1);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'(dut.RX_DEPTH));
        if (eof_at_capacity) begin
            `assert_equal(dut.talk_event_pending, 1'b1);
            ctl_atn(1);
            ctl_atn(0);
            `assert_equal(dut.talk_event_pending, 1'b1);
            `assert_equal(dut.atn_end_pending, 1'b1);
        end
        // A real firmware pause must not get trapped behind an unaccepted byte.
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | IEEE_TX_OWNER_PAUSE_MASK | 8'h02);
        repeat (STALL_CYCLES) @(posedge sys_clock);
        `assert_equal(dut.dev_dav_n, 1'b1);
        mcu_drain_rx;
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | 8'h02);
        repeat (STALL_CYCLES) @(posedge sys_clock);
        mcu_drain_rx;
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'(record_length));
        `assert_equal(rx_bytes.size(), first_data + record_length);
        `assert_equal(rx_talk_events.size(), previous_events + 1);
        `assert_equal(rx_talk_events[previous_events], 8'hC2);
        if (eof_at_capacity) begin
            `assert_equal(rx_metadata_tags[rx_metadata_tags.size() - 2], 8'h40);
            `assert_equal(rx_metadata_tags[rx_metadata_tags.size() - 1], 8'h10);
        end
        for (int i = 0; i < record_length; ++i) begin
            `assert_equal(rx_bytes[first_data + i], 8'(i + 1));
            `assert_equal(rx_isatn[first_data + i], 1'b0);
            `assert_equal(rx_iseoi[first_data + i], (i == record_length - 1));
        end
        ctl_atn(1);
        ctl_send(8'h3F);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    // Owner mismatches block either source before firmware service.
    task automatic check_data_owner(input bit other_unit);
        logic [7:0] d;
        logic eoi;
        localparam logic [7:0] OWNER_CHANNEL_ONE = IEEE_TX_OWNER_VALID_MASK | 8'h01;
        $display("[%t]   data owner other_unit=%b", $time, other_unit);
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        mcu_write(IEEE_REG_SA, OWNER_CHANNEL_ONE);
        mcu_write(IEEE_REG_TX, "X");
        mcu_write(IEEE_REG_TX_LAST, "Y");
        ctl_atn(1);
        ctl_send(other_unit ? 8'h49 : 8'h48);
        ctl_send(other_unit ? 8'h61 : 8'h62);
        ctl_ndac(1);
        ctl_nrfd(0);
        ctl_atn(0);
        repeat (20) @(posedge sys_clock);
        `assert_equal(dut.dev_dav_n, 1'b1);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd0);

        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_send(8'h48);
        ctl_send(8'h61);
        ctl_atn(0);
        ctl_recv(d, eoi);
        `assert_equal(d, "X");
        `assert_equal(eoi, 1'b0);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd1);
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_DATA_FLUSH_MASK);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd1);

        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_send(8'h29);
        ctl_send(8'hF2);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd1);
        ctl_send(8'h3F);
        ctl_send(8'h49);
        ctl_send(8'h6F);
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | 8'h1F);
        mcu_write(IEEE_REG_TXS_LAST, "S");
        ctl_atn(0);
        ctl_recv(d, eoi);
        `assert_equal(d, "S");
        `assert_equal(eoi, 1'b1);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd1);
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    // Parking holds either source through NRFD and NDAC, without an extra GET.
    task automatic check_owner_pause(input bit prefetch, input bit status_fifo);
        logic [7:0] d;
        logic eoi;
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | (status_fifo ? 8'h0F : 8'h01));
        mcu_write(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, "X");
        mcu_write(status_fifo ? IEEE_REG_TXS_LAST : IEEE_REG_TX_LAST, "Y");
        ctl_atn(1);
        ctl_send(8'h48);
        ctl_send(status_fifo ? 8'h6F : 8'h61);
        if (status_fifo) begin
            mcu_write(IEEE_REG_TXS, "X");
            mcu_write(IEEE_REG_TXS_LAST, "Y");
        end
        ctl_ndac(1);
        ctl_nrfd(0);
        ctl_atn(0);
        ctl_wait(8'h80, 8'h00);
        if (prefetch) ctl_nrfd(1);
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | IEEE_TX_OWNER_PAUSE_MASK);
        mcu_read(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, d);
        `assert_equal(d, prefetch ? 8'd1 : 8'd0);
        `assert_equal(dut.dev_dav_n, 1'b0);
        `assert_equal(dut.dev_dio, ~8'("X"));
        `assert_equal(dut.dev_eoi_n, 1'b1);
        `assert_equal((status_fifo ? dut.txs_count : dut.tx_count), prefetch ? 1 : 2);
        mcu_read(IEEE_REG_CTRL, d);
        `assert_equal((d & (status_fifo ? IEEE_CTRL_RD_STATUS_ACTIVE_MASK : IEEE_CTRL_RD_DATA_ACTIVE_MASK)),
            (status_fifo ? IEEE_CTRL_RD_STATUS_ACTIVE_MASK : IEEE_CTRL_RD_DATA_ACTIVE_MASK));
        mcu_read(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, d);
        `assert_equal(d, prefetch ? 8'd1 : 8'd0);
        if (!prefetch) begin
            ctl_nrfd(1);
            mcu_read(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, d);
            `assert_equal(d, 8'd1);
            `assert_equal(dut.dev_dav_n, 1'b0);
            `assert_equal(dut.dev_dio, ~8'("X"));
        end
        ctl_ndac(0);
        ctl_wait(8'h80, 8'h80);
        mcu_read(IEEE_REG_CTRL, d);
        `assert_equal((d & (IEEE_CTRL_RD_STATUS_ACTIVE_MASK | IEEE_CTRL_RD_DATA_ACTIVE_MASK)), 8'd0);
        ctl_ndac(1);
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | (status_fifo ? 8'h0F : 8'h01));
        ctl_recv(d, eoi);
        `assert_equal(d, "Y");
        `assert_equal(eoi, 1'b1);
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    // Ready listeners cannot consume a peer's status or bytes parked by firmware.
    task automatic check_status_owner;
        logic [7:0] d;
        logic eoi;
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        mcu_write(IEEE_REG_TX_LAST, "D");
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | 8'h0F);
        ctl_atn(1);
        ctl_send(8'h49);
        ctl_send(8'h6F);
        mcu_write(IEEE_REG_TXS_LAST, "S");
        ctl_ndac(1);
        ctl_nrfd(0);
        ctl_atn(0);
        repeat (32) @(posedge sys_clock);
        `assert_equal(dut.dev_dav_n, 1'b1);
        mcu_read(IEEE_REG_TXS, d);
        `assert_equal(d, 8'd0);
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | IEEE_TX_OWNER_PAUSE_MASK);
        repeat (32) @(posedge sys_clock);
        `assert_equal(dut.dev_dav_n, 1'b1);
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | 8'h1F);
        ctl_recv(d, eoi);
        `assert_equal(d, "S");
        `assert_equal(eoi, 1'b1);
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | IEEE_TX_OWNER_PAUSE_MASK);
        mcu_write(IEEE_REG_TXS_LAST, "X");
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_STATUS_FLUSH_MASK);
        mcu_read(IEEE_REG_TXS, d);
        `assert_equal(d, 8'd1);
        `assert_equal(dut.txs_count, 0);
        `assert_equal(dut.tx_count, 1);
        mcu_write(IEEE_REG_TXS_LAST, "T");
        mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK | 8'h1F);
        ctl_recv(d, eoi);
        `assert_equal(d, "T");
        `assert_equal(eoi, 1'b1);
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    // Coincident discard cannot erase GET progress for the already-presented byte.
    task automatic check_prefetch_flush_collision;
        logic [7:0] d;
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        mcu_write(IEEE_REG_TX, "X");
        mcu_write(IEEE_REG_TX_LAST, "Y");
        ctl_atn(1);
        ctl_send(8'h48);
        ctl_send(8'h61);
        ctl_ndac(1);
        ctl_nrfd(0);
        ctl_atn(0);
        ctl_wait(8'h80, 8'h00);
        fork
            ctl_nrfd(1);
            begin
                // Schedule the external Wishbone discard with the CPU's NRFD write.
                wait (cpu_data_strobe && dut.snap_via && dut.snap_we
                    && dut.snap_rs == 0 && !cpu_data[1]);
                @(negedge sys_clock);
                wb_addr = common_pkg::wb_ieee_addr(IEEE_REG_CTRL);
                wb_dout = IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_DATA_FLUSH_MASK;
                wb_we = 1;
                wb_cycle = 1;
                wb_strobe = 1;
                @(negedge sys_clock);
                wb_we = 0;
                wb_cycle = 0;
                wb_strobe = 0;
            end
        join
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd1);
        `assert_equal(dut.tx_count, 0);
        `assert_equal(dut.dev_dav_n, 1'b0);
        `assert_equal(dut.dev_dio, ~8'h58);
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    // Counter wrap remains observable without resetting FIFO pointers or ownership.
    task automatic check_prefetch_wrap(input bit status_fifo);
        logic [7:0] d;
        logic eoi;
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        ctl_atn(1);
        ctl_send(8'h48);
        ctl_send(status_fifo ? 8'h6F : 8'h60);
        ctl_atn(0);
        for (int i = 0; i < 257; ++i) begin
            mcu_write(status_fifo ? IEEE_REG_TXS_LAST : IEEE_REG_TX_LAST, 8'(i));
            ctl_recv(d, eoi);
            `assert_equal(d, 8'(i));
            `assert_equal(eoi, 1'b1);
            mcu_read(status_fifo ? IEEE_REG_TXS : IEEE_REG_TX, d);
            `assert_equal(d, 8'(i + 1));
            mcu_drain_rx;
        end
        ctl_atn(1);
        ctl_send(8'h5F);
        ctl_atn(0);
        mcu_drain_rx;
    endtask

    task static run;
        logic [7:0] d, st;
        logic eoi;
        string name;

        $display("[%t] BEGIN IEEE drive emulation test", $time);

        // Initialize IFC-resettable DUT state before the first test.
        ifc = 1'b1;
        repeat (2) @(posedge sys_clock);
        ifc = 1'b0;
        repeat (2) @(posedge sys_clock);

        // MCU: enable + flush
        mcu_write(IEEE_REG_CTRL, 8'h03);
        mcu_write(IEEE_REG_CTRL, 8'h01);

        // CTRL flush clears every FIFO, while data-only flush preserves the
        // status FIFO. The data FIFO advertises room only for complete
        // 64-byte bursts and accepts exactly its advertised capacity.
        mcu_write(IEEE_REG_TX, 8'hA0);
        mcu_write(IEEE_REG_TXS, 8'hB0);
        mcu_write(IEEE_REG_CTRL, 8'h03);
        @(posedge sys_clock);
        `assert_equal(dut.rx_count, 0);
        `assert_equal(dut.tx_count, 0);
        `assert_equal(dut.txs_count, 0);

        mcu_write(IEEE_REG_TX, 8'hA0);
        mcu_write(IEEE_REG_TXS, 8'hB0);
        mcu_write(IEEE_REG_CTRL, 8'h05);
        @(posedge sys_clock);
        `assert_equal(dut.tx_count, 0);
        `assert_equal(dut.txs_count, 1);
        mcu_write(IEEE_REG_CTRL, 8'h03);
        @(posedge sys_clock);

        for (int i = 0; i < 960; i++) mcu_write(IEEE_REG_TX, i[7:0]);
        mcu_read(IEEE_REG_CTRL, d);
        `assert_equal(d[1], 1'b1);
        mcu_write(IEEE_REG_TX, 8'hC0);
        mcu_read(IEEE_REG_CTRL, d);
        `assert_equal(d[1], 1'b0);
        for (int i = 0; i < 63; i++) mcu_write(IEEE_REG_TX, i[7:0]);
        mcu_read(IEEE_REG_STATUS, st);
        `assert_equal(st[2], 1'b1);
        mcu_write(IEEE_REG_CTRL, 8'h05);
        @(posedge sys_clock);
        mcu_read(IEEE_REG_STATUS, st);
        `assert_equal(st[2], 1'b0);
        `assert_equal(st[3], 1'b1);

        // Kernel-style init of controller-side registers
        cpu_write(0, 1, 0, 4'd1, 8'h3C);   // PIA2 CRA: CA2 high (NDAC released)
        cpu_write(0, 1, 0, 4'd3, 8'h3C);   // PIA2 CRB: CB2 high (DAV released)
        cpu_write(0, 1, 0, 4'd2, 8'hFF);   // DIO released
        cpu_write(0, 0, 1, 4'd0, 8'hFF);   // VIA ORB: ATN/NRFD released
        ctl_eoi(0);

        // RX metadata belongs to each captured DAV edge, not the live EOI line.
        ctl_atn(1);
        ctl_send(8'h28);
        ctl_send(8'hFF);
        ctl_atn(0);
        ctl_send(8'h41);
        ctl_eoi(1);
        ctl_send(8'h00);
        ctl_eoi(0);
        ctl_send(8'h42);
        ctl_atn(1);
        ctl_send(8'h3F);
        ctl_atn(0);
        mcu_drain_rx;
        `assert_equal(rx_bytes.size(), 6);
        for (int i = 0; i < 6; i++) begin
            `assert_equal(rx_iseoi[i], (i == 3));
            `assert_equal(rx_isatn[i], (i < 2 || i == 5));
        end
        `assert_equal(rx_bytes[3], 8'h00);
        mcu_read(IEEE_REG_CTRL, d);
        `assert_equal((d & IEEE_CTRL_RD_RX_EOI_MASK), 8'd0);
        rx_bytes.delete(); rx_isatn.delete(); rx_iseoi.delete();

        // Status and file data use separate FIFOs. A status transaction must
        // not consume the queued file bytes, and each LAST register applies
        // EOI only to its respective FIFO.
        mcu_write(IEEE_REG_TX, 8'hA0);
        mcu_write(IEEE_REG_TX_LAST, 8'hA1);
        ctl_atn(1);
        ctl_send(8'h48);                   // TALK 8
        ctl_send(8'h6F);                   // secondary 15
        ctl_atn(0);
        mcu_push("S", 1);
        ctl_recv(d, eoi);
        `assert_equal(d, "S");
        `assert_equal(eoi, 1'b0);
        ctl_recv(d, eoi);
        `assert_equal(d, 8'h0D);
        `assert_equal(eoi, 1'b1);
        ctl_atn(1);
        ctl_send(8'h5F);                   // UNTALK
        ctl_send(8'h48);                   // TALK 8
        ctl_send(8'h60);                   // secondary 0
        ctl_atn(0);
        ctl_recv(d, eoi);
        `assert_equal(d, 8'hA0);
        `assert_equal(eoi, 1'b0);
        ctl_recv(d, eoi);
        `assert_equal(d, 8'hA1);
        `assert_equal(eoi, 1'b1);
        ctl_atn(1);
        ctl_send(8'h5F);                   // UNTALK
        ctl_atn(0);
        mcu_drain_rx;
        rx_bytes.delete();
        rx_isatn.delete();

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

        // RX reads must be side-effect-free because SPI prefetches the next
        // register. Only a write to RX pops the current tagged entry.
        mcu_read(IEEE_REG_STATUS, st);
        `assert_equal(st[0], 1'b1);
        `assert_equal(st[1], 1'b1);
        mcu_read(IEEE_REG_RX, d);
        `assert_equal(d, 8'h28);
        mcu_read(IEEE_REG_RX, d);
        `assert_equal(d, 8'h28);
        mcu_write(IEEE_REG_RX, 8'h00);
        mcu_read(IEEE_REG_STATUS, st);
        `assert_equal(st[0], 1'b1);
        `assert_equal(st[1], 1'b1);
        mcu_read(IEEE_REG_RX, d);
        `assert_equal(d, 8'hF0);

        // MCU sees the remaining tagged command/data stream.
        mcu_drain_rx;
        `assert_equal(rx_bytes.size(), 13);
        `assert_equal(rx_isatn[0], 1);  `assert_equal(rx_bytes[0], 8'hF0);
        `assert_equal(rx_isatn[1], 0);  `assert_equal(rx_bytes[1], "1");
        `assert_equal(rx_isatn[11], 0); `assert_equal(rx_bytes[11], "G");
        `assert_equal(rx_isatn[12], 1); `assert_equal(rx_bytes[12], 8'h3F);
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
            string status_str;
            status_str = "";
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
        // without asserting NRFD or reading DIO, waits for DAV to clear,
        // and only then asserts ATN. Native TLK20 has not entered GET for
        // byte 10, so the resume loop below must still start at 10.
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
        // Fill RX (2 command bytes + ATN end + 29 data), then send one
        // more data byte with no room; the device must hold NDAC (not ACK,
        // not drop) until the MCU drains, then complete the handshake.
        rx_bytes.delete(); rx_isatn.delete();
        rx_iseoi.delete();
        ctl_atn(1);
        ctl_send(8'h28);                   // LISTEN 8
        ctl_send(8'hF1);                   // OPEN ch1
        ctl_atn(0);
        for (int i = 0; i < 29; i++) ctl_send(8'h20 + i[7:0]);
        ctl_eoi(1);
        fork
            ctl_send(8'hA5);               // stalls at the full FIFO...
            begin
                #5us;                      // ...until the MCU drains it
                mcu_drain_rx;
            end
        join
        ctl_eoi(0);
        mcu_drain_rx;                      // collect the A5 pushed after drain
        `assert_equal(rx_bytes.size(), 32);
        `assert_equal(rx_bytes[31], 8'hA5);
        `assert_equal(rx_bytes[30], 8'h3C); // 0x20+28
        `assert_equal(rx_iseoi[30], 1'b0);
        `assert_equal(rx_iseoi[31], 1'b1);
        ctl_atn(1);
        ctl_send(8'h3F);                   // UNLISTEN
        ctl_atn(0);
        mcu_drain_rx;
        $display("[%t]   RX-full backpressure verified", $time);

        // ATN release itself must survive a full FIFO and retain stream order.
        begin
            int previous_boundaries, previous_bytes;
            previous_boundaries = rx_boundary_count;
            previous_bytes = rx_bytes.size();
            ctl_atn(1);
            for (int i = 0; i < dut.RX_DEPTH; i++) ctl_send(8'h3F);
            ctl_atn(0);
            `assert_equal(dut.atn_end_pending, 1'b1);
            mcu_drain_rx;
            `assert_equal(rx_boundary_count, previous_boundaries + 1);
            `assert_equal(rx_bytes.size(), previous_bytes + dut.RX_DEPTH);
            `assert_equal(dut.atn_end_pending, 1'b0);
        end

        // Cover both prefetch phases, with/without private reads, on each FIFO.
        check_talk_event(1'b0, 1'b0, 1'b0);
        check_talk_event(1'b1, 1'b0, 1'b0);
        check_talk_event(1'b0, 1'b1, 1'b0);
        check_talk_event(1'b0, 1'b1, 1'b1);
        check_talk_event(1'b1, 1'b1, 1'b0);
        begin
            localparam int ABORT_SCENARIOS = 8;
            for (int scenario = 0; scenario < ABORT_SCENARIOS; scenario++) begin
                check_talk_abort(scenario[2], scenario[1], scenario[0]);
            end
        end
        $display("[%t]   NRFD/private-read abort matrix verified", $time);

        // --- dense counted-read + terminate + resume (the Super-OS/9 REL
        // per-record pattern) at a tight 1MHz cadence ---
        // The kernel reads a counted number of bytes then terminates mid-stream
        // (release NDAC with NRFD still released and no next-byte DIO read),
        // UNTALKs, and re-TALKs to continue. At the packed cadence the fast
        // talker already has the next byte's DAV up during terminate. Without
        // NRFD assertion it must NOT prefetch, or the resume is off-by-one.
        // Several records: the race is timing-dependent.
        begin
            int saved_tail;
            saved_tail = g_tail;
            for (int rec = 0; rec < 6; rec++) begin
                int cnt;
                cnt = 8 + rec;                // vary the counted length per record
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

        // A virtual peer's secondary belongs to that peer, not the active talker.
        for (int unit = 0; unit < 4; unit++) begin
            for (int peer = 0; peer < 4; peer++) begin
                if (peer != unit) begin
                    mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
                    mcu_push("", 1);
                    mcu_write(IEEE_REG_SA, IEEE_TX_OWNER_VALID_MASK
                        | (unit << IEEE_TX_OWNER_UNIT_SHIFT) | 8'h02);
                    mcu_write(IEEE_REG_TX, 8'hA5);
                    mcu_write(IEEE_REG_TX_LAST, 8'h5A);
                    ctl_atn(1);
                    ctl_send(8'h48 + unit[7:0]);
                    ctl_send(8'h62);
                    ctl_send(8'h28 + peer[7:0]);
                    ctl_send(8'h6F);
                    ctl_send(8'h3F);
                    `assert_equal(dut.talking, 1'b1);
                    `assert_equal(dut.txs_count, 1);
                    ctl_atn(0);
                    ctl_recv(d, eoi);
                    `assert_equal(d, 8'hA5);
                    `assert_equal(eoi, 1'b0);
                    ctl_recv(d, eoi);
                    `assert_equal(d, 8'h5A);
                    `assert_equal(eoi, 1'b1);
                    ctl_atn(1); ctl_send(8'h5F); ctl_atn(0);
                    mcu_drain_rx;
                end
            end
        end
        // Peripherals retain independent listener roles and primary selection.
        for (int unit = 0; unit < 4; unit++) begin
            int peer;
            peer = (unit + 1) % 4;
            ctl_atn(1);
            ctl_send(8'h28 + unit[7:0]);
            ctl_send(8'h6F);
            ctl_send(8'h28 + peer[7:0]);
            ctl_send(8'h62);
            ctl_send(8'h48 + peer[7:0]);
            `assert_equal(dut.listen_units[unit], 1'b1);
            `assert_equal(dut.listen_units[peer], 1'b0);
            ctl_send(8'h28 + peer[7:0]);
            `assert_equal(dut.talking, 1'b0);
            ctl_send(8'h27);
            ctl_send(8'h61);
            `assert_equal(dut.unit_sa[unit], 8'h6F);
            `assert_equal(dut.unit_sa[peer], 8'h20);
            ctl_send(8'h5F);
            `assert_equal(dut.listening, 1'b1);
            ctl_send(8'h3F);
            `assert_equal(dut.listening, 1'b0);
            ctl_atn(0);
            mcu_drain_rx;
        end
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);

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

        // --- Unit 11: the four-address range also answers device 11. ---
        ctl_atn(1);
        ctl_send(8'h2B);                   // LISTEN 11
        ctl_send(8'h60);                   // secondary 0
        ctl_atn(0);
        ctl_send(8'h5A);
        ctl_atn(1);
        ctl_send(8'h3F);                   // UNLISTEN
        ctl_atn(0);
        begin
            int base;
            base = rx_bytes.size();
            mcu_drain_rx;
            `assert_equal(rx_bytes.size(), base + 4);
            `assert_equal(rx_isatn[base + 0], 1); `assert_equal(rx_bytes[base + 0], 8'h2B);
            `assert_equal(rx_isatn[base + 1], 1); `assert_equal(rx_bytes[base + 1], 8'h60);
            `assert_equal(rx_isatn[base + 2], 0); `assert_equal(rx_bytes[base + 2], 8'h5A);
            `assert_equal(rx_isatn[base + 3], 1); `assert_equal(rx_bytes[base + 3], 8'h3F);
        end
        mcu_write(IEEE_REG_TX_LAST, 8'h7B);
        ctl_atn(1);
        ctl_send(8'h4B);                   // TALK 11
        ctl_send(8'h60);                   // secondary 0
        ctl_atn(0);
        ctl_recv(d, eoi);
        `assert_equal(d, 8'h7B);
        `assert_equal(eoi, 1'b1);
        ctl_atn(1);
        ctl_send(8'h5F);                   // UNTALK
        ctl_atn(0);
        mcu_drain_rx;
        $display("[%t]   unit 11 listen/talk verified", $time);
        check_data_owner(1'b0);
        check_peer_transfer(1'b0);
        check_peer_transfer(1'b1);
        check_peer_backpressure(1'b0, 1'b0);
        check_peer_backpressure(1'b1, 1'b0);
        check_peer_backpressure(1'b0, 1'b1);
        check_data_owner(1'b1);
        check_owner_pause(1'b0, 1'b0);
        check_owner_pause(1'b1, 1'b0);
        check_owner_pause(1'b0, 1'b1);
        check_owner_pause(1'b1, 1'b1);
        check_status_owner;
        check_prefetch_flush_collision;
        check_prefetch_wrap(0);
        check_prefetch_wrap(1);
        mcu_write(IEEE_REG_CTRL, IEEE_CTRL_ENABLE_MASK | IEEE_CTRL_FLUSH_MASK);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd0);

        // IFC must clear a live protocol exchange. IFC deassertion must
        // neither restore stale outputs nor create an RX entry.
        rx_bytes.delete();
        rx_isatn.delete();
        mcu_write(IEEE_REG_TX, 8'hA5);
        mcu_push("S", 1);
        ctl_atn(1);
        ctl_send(8'h28);                   // LISTEN 8
        ctl_send(8'h61);                   // secondary 1
        ctl_atn(0);
        ctl_send(8'h55);                   // listener data: RX is non-empty
        `assert_compare(dut.rx_count, >, 0);
        `assert_compare(dut.tx_count, >, 0);
        `assert_compare(dut.txs_count, >, 0);
        `assert_equal(dut.listening, 1'b1);

        ifc = 1'b1;
        repeat (2) @(posedge sys_clock);
        mcu_read(IEEE_REG_CTRL, d);
        `assert_equal(d[0], 1'b1);         // enable survives IFC
        `assert_equal(dut.rx_count, 0);
        `assert_equal(dut.tx_count, 0);
        `assert_equal(dut.txs_count, 0);
        `assert_equal(dut.listening, 1'b0);
        `assert_equal(dut.talking, 1'b0);
        `assert_equal(dut.sa, 8'h00);
        mcu_read(IEEE_REG_TX, d);
        `assert_equal(d, 8'd0);
        `assert_equal(dut.tx_owner, 8'd0);
        `assert_equal(dut.talk_st, dut.T_IDLE);
        `assert_equal(dut.dev_nrfd_n, 1'b1);
        `assert_equal(dut.dev_ndac_n, 1'b1);
        `assert_equal(dut.dev_dav_n, 1'b1);
        `assert_equal(dut.dev_eoi_n, 1'b1);
        `assert_equal(dut.dev_dio, 8'hFF);
        `assert_equal(dut.cpu_data_oe, 1'b0);

        ifc = 1'b0;
        repeat (2) @(posedge sys_clock);
        mcu_read(IEEE_REG_STATUS, st);
        `assert_equal(st[0], 1'b0);        // no phantom command after IFC

        // The PET reinitializes its PIA/VIA after RES before its first command.
        cpu_write(0, 1, 0, 4'd1, 8'h3C);
        cpu_write(0, 1, 0, 4'd3, 8'h3C);
        cpu_write(0, 1, 0, 4'd2, 8'hFF);
        cpu_write(0, 0, 1, 4'd0, 8'hFF);
        ctl_atn(1);
        ctl_send(8'h28);                   // first post-IFC command
        ctl_atn(0);
        mcu_drain_rx;
        `assert_equal(rx_bytes.size(), 1);
        `assert_equal(rx_isatn[0], 1'b1);
        `assert_equal(rx_bytes[0], 8'h28);
        ctl_atn(1);
        ctl_send(8'h3F);                   // UNLISTEN
        ctl_atn(0);
        mcu_drain_rx;
        $display("[%t]   IFC reset and post-reset recovery verified", $time);

        // Idle: device releases everything
        mcu_read(IEEE_REG_STATUS, st);
        `assert_equal(st[6], 0);           // not talking
        `assert_equal(st[5], 0);           // not listening

        $display("[%t] END IEEE drive emulation test", $time);
    endtask

    `TB_INIT
endmodule
