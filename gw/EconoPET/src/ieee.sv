// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// IEEE-488 disk-drive emulation (devices 8 through 11).
//
// Sits between the selected CPU and the physical PIA1/PIA2/VIA the same way
// keyboard.sv sits in front of PIA1: it snoops CPU writes to the IEEE-related
// I/O registers and, when enabled, suppresses direction/control writes that
// would enable external IEEE drivers while injecting virtual IEEE reads.
// Firmware parks VIA PB2:1 (ATN/NRFD) and PIA2 (DIO/NDAC/DAV) as inputs over
// the I/O bridge while halted, so ORA/ORB data writes still reach the physical
// chips for cassette and latch tracking without driving the external connector.
// Disabled it is fully passive (physical devices are untouched).
//
// PET IEEE-488 register map:
//   PIA2 $E820 PA/DDRA = DIO in        (snoop and suppress write, inject bus_dio or DDRA read)
//   PIA2 $E821 CRA     = CA2 -> NDAC   (snoop and suppress write, inject CRA[5:0] read)
//   PIA2 $E822 PB/DDRB = DIO out       (snoop and suppress write, inject cpu_dio or DDRB read)
//   PIA2 $E823 CRB     = CB2 -> DAV    (snoop and suppress write, inject CRB[5:0] read)
//   VIA  $E840 PB      = bit0 NDAC in, bit1 NRFD out, bit2 ATN out,
//                        bit6 NRFD in, bit7 DAV in (ORB write passes through, inject read)
//   VIA  $E842 DDRB    = Port B dirs   (snoop and suppress write, inject DDRB read)
//   PIA1 $E810 PA      = bit6 EOI in   (inject only while DAV or EOI is asserted)
//
module ieee (
    input  logic wb_clock_i,
    input  logic ifc_i,

    // Wishbone peripheral (MCU side)
    input  logic [WB_ADDR_WIDTH-1:0] wbp_addr_i,
    input  logic [   DATA_WIDTH-1:0] wbp_data_i,
    output logic [   DATA_WIDTH-1:0] wbp_data_o,
    input  logic                     wbp_we_i,
    input  logic                     wbp_cycle_i,
    input  logic                     wbp_strobe_i,
    output logic                     wbp_stall_o,
    output logic                     wbp_ack_o,
    input  logic                     wbp_sel_i,

    // CPU bus snoop/inject
    input  logic                     cpu_be_i,
    input  logic                     cpu_addr_strobe_i,
    input  logic                     cpu_data_strobe_i,
    input  logic [   DATA_WIDTH-1:0] cpu_data_i,
    output logic [   DATA_WIDTH-1:0] cpu_data_o,
    output logic                     cpu_data_oe,
    input  logic                     cpu_we_i,

    input  logic                     pia1_cs_i,
    input  logic                     pia2_cs_i,
    input  logic                     via_cs_i,
    input  logic [ VIA_RS_WIDTH-1:0] rs_i,          // cpu_addr[3:0]
    output logic                     claim_pia1_o,  // suppress external PIA1 CS and /IO_OE on claimed cycle
    output logic                     claim_pia2_o,  // suppress external PIA2 CS and /IO_OE on claimed cycle
    output logic                     claim_via_o,   // suppress external VIA CS and /IO_OE on claimed cycle

    input  logic                     diag_i,        // PIA1 PA7 (physical pin)
    input  logic                     vert_i         // vertical drive, for VIA PB5
);
    // ------------------------------------------------------------------
    // Control / status registers
    // ------------------------------------------------------------------
    logic flush = 1'b0;
    // Active-high emulated-device presence. No units means physical passthrough.
    logic [3:0] unit_mask = '0;
    wire enable = |unit_mask;

    // ------------------------------------------------------------------
    // Snooped CPU-side IEEE line states (all _n: 1 = released)
    // ------------------------------------------------------------------
    logic [7:0] pia2_ora    = 8'h00;   // snooped guest PIA2 ORA (DIO in output latch)
    logic [7:0] pia2_ddra   = 8'h00;   // snooped guest PIA2 DDRA
    logic [7:0] pia2_pb_out = 8'h00;   // snooped guest PIA2 ORB (active-low DIO out latch)
    logic [7:0] pia2_ddrb   = 8'h00;   // snooped guest PIA2 DDRB
    logic [7:0] pia2_cra    = 8'h00;
    logic [7:0] pia2_crb    = 8'h00;
    logic [7:0] via_orb     = 8'h00;   // snooped guest VIA ORB
    logic [7:0] via_ddrb    = 8'h00;   // snooped guest VIA DDRB
    logic [3:0] pia1_pa_out = 4'h0;    // keyboard column select (for $E810 compose)
    logic [7:0] pia1_cra = 8'h00;
    logic [1:0] cassette_sense = 2'b11;

    // Pins configured as inputs (DDR bit == 0) float high on the open-collector IEEE bus.
    wire [7:0] cpu_dio = pia2_pb_out | ~pia2_ddrb;
    wire cpu_eoi_n  = (pia1_cra[5:4] == 2'b11) ? pia1_cra[3] : 1'b1;
    wire cpu_ndac_n = (pia2_cra[5:4] == 2'b11) ? pia2_cra[3] : 1'b1;
    wire cpu_dav_n  = (pia2_crb[5:4] == 2'b11) ? pia2_crb[3] : 1'b1;
    wire cpu_atn_n  = via_orb[2] | ~via_ddrb[2];
    wire cpu_nrfd_n = via_orb[1] | ~via_ddrb[1];

    // Per-bus-cycle snapshot of the decode: the CPU changes ADDR/RnW at
    // E's falling edge while the bus-enable tail is still live, so live
    // decode can disagree with the registered chip-selects late in the
    // cycle. Everything below decides from this snapshot. The address
    // strobe asserts one sys clock before cpu_addr_i is valid and the
    // chip-selects register one clock later still, hence the 3-clock delay.
    logic snap_pia1, snap_pia2, snap_via;
    logic [VIA_RS_WIDTH-1:0] snap_rs;
    logic snap_we;
    logic [2:0] snap_dly = '0;

    // snap_valid: high from the snapshot instant until the next cycle's
    // address strobe. Injection below only acts while it's high, so the
    // stale previous-cycle snapshot can never briefly inject (or hold OE
    // into the early clocks of an unrelated following cycle, which would
    // mask the physical PIA/VIA chip-selects in main.sv).
    logic snap_valid = 1'b0;

    always_ff @(posedge wb_clock_i) begin
        if (ifc_i) begin
            snap_pia1  <= 1'b0;
            snap_pia2  <= 1'b0;
            snap_via   <= 1'b0;
            snap_rs    <= '0;
            snap_we    <= 1'b0;
            snap_dly   <= '0;
            snap_valid <= 1'b0;
        end else begin
            // Gate snap_dly and snap_valid with cpu_be_i so MCU io_ctl bridge cycles
            // (when cpu_be_i == 0) never overwrite the snooped guest I/O shadows.
            snap_dly <= {snap_dly[1:0], cpu_addr_strobe_i && cpu_be_i};
            // BE fall ends the cycle (a stale snapshot must not inject into the next).
            if (cpu_addr_strobe_i || !cpu_be_i) snap_valid <= 1'b0;
            if (snap_dly[2] && cpu_be_i) begin
                snap_valid <= 1'b1;
                snap_pia1  <= pia1_cs_i;
                snap_pia2  <= pia2_cs_i;
                snap_via   <= via_cs_i;
                snap_rs    <= rs_i;
                snap_we    <= cpu_we_i;
            end
        end
    end

    wire [PIA_RS_WIDTH-1:0] pia_rs = snap_rs[PIA_RS_WIDTH-1:0];
    wire cpu_wr = cpu_data_strobe_i && snap_valid && snap_we;

    // Retain physical cassette switches during injected reads. Only an unclaimed
    // guest ORA read observes the pins (not DDR/control reads or our own injection).
    always_ff @(posedge wb_clock_i) begin
        if (ifc_i) cassette_sense <= 2'b11;
        else if (cpu_data_strobe_i && cpu_be_i && snap_valid && !snap_we &&
                 snap_pia1 && pia_rs == 2'd0 && pia1_cra[2] && !claim_pia1_o)
            cassette_sense <= cpu_data_i[5:4];
    end

    // Snoop guest CPU writes (even while disabled) so firmware can park or restore physical PIA2/VIA cleanly.
    always_ff @(posedge wb_clock_i) begin
        if (ifc_i) begin
            pia2_ora    <= 8'h00;
            pia2_ddra   <= 8'h00;
            pia2_pb_out <= 8'h00;
            pia2_ddrb   <= 8'h00;
            pia2_cra    <= 8'h00;
            pia2_crb    <= 8'h00;
            via_orb     <= 8'h00;
            via_ddrb    <= 8'h00;
            pia1_pa_out <= 4'h0;
            pia1_cra    <= 8'h00;
        end else if (cpu_wr) begin
            if (snap_pia2) begin
                unique case (pia_rs)
                    2'd0: if (pia2_cra[2]) pia2_ora <= cpu_data_i; else pia2_ddra <= cpu_data_i;
                    2'd1: pia2_cra <= cpu_data_i;
                    2'd2: if (pia2_crb[2]) pia2_pb_out <= cpu_data_i; else pia2_ddrb <= cpu_data_i;
                    2'd3: pia2_crb <= cpu_data_i;
                    default: ;
                endcase
            end
            if (snap_via && snap_rs == 4'd0) via_orb  <= cpu_data_i;
            if (snap_via && snap_rs == 4'd2) via_ddrb <= cpu_data_i;
            if (snap_pia1 && pia_rs == 2'd0 && pia1_cra[2]) pia1_pa_out <= cpu_data_i[3:0];
            if (snap_pia1 && pia_rs == 2'd1) pia1_cra <= cpu_data_i;
        end
    end

    // ------------------------------------------------------------------
    // Device-side lines (1 = released)
    // ------------------------------------------------------------------
    logic dev_nrfd_n = 1'b1;
    logic dev_ndac_n = 1'b1;
    logic dev_dav_n  = 1'b1;
    logic dev_eoi_n  = 1'b1;
    logic [7:0] dev_dio = 8'hFF;       // active-low value we drive during talk

    wire bus_dav_n  = cpu_dav_n  & dev_dav_n;
    wire bus_ndac_n = cpu_ndac_n & dev_ndac_n;
    wire bus_nrfd_n = cpu_nrfd_n & dev_nrfd_n;
    wire bus_eoi_n = cpu_eoi_n & dev_eoi_n;
    wire [7:0] bus_dio = cpu_dio & dev_dio;

    // ------------------------------------------------------------------
    // FIFOs
    //   RX: CPU -> MCU, {talk_event, atn_end, eoi, atn, data}.
    //   TX: MCU -> CPU, 9-bit entries {eoi, data}
    // ------------------------------------------------------------------
    localparam RX_DEPTH = 32;
    // Must hold more than one firmware main-loop period of drained bytes or
    // the kernel's counted read underruns mid-record; 1024 covers a loop
    // period at the 1MHz drain rate. The combinational read port maps this
    // to LUT-RAM, so depth is LUT-bounded.
    localparam TX_DEPTH = 1024;

    logic [11:0] rx_mem [RX_DEPTH-1:0];
    logic [$clog2(RX_DEPTH)-1:0] rx_wr = '0, rx_rd = '0;
    logic [$clog2(RX_DEPTH):0]   rx_count = '0;
    wire rx_empty = rx_count == 0;
    wire rx_full  = rx_count == ($clog2(RX_DEPTH)+1)'(RX_DEPTH);
    wire rx_room = !rx_full
        && !(rx_push && rx_count == ($clog2(RX_DEPTH)+1)'(RX_DEPTH - 1));

    logic [8:0] tx_mem [TX_DEPTH-1:0];
    logic [$clog2(TX_DEPTH)-1:0] tx_wr = '0, tx_rd = '0;
    logic [$clog2(TX_DEPTH):0]   tx_count = '0;
    wire tx_empty = tx_count == 0;
    wire tx_full  = tx_count == ($clog2(TX_DEPTH)+1)'(TX_DEPTH);
    logic [7:0] tx_prefetched = '0;
    logic [7:0] txs_prefetched = '0;
    logic [7:0] tx_owner = '0;

    // Burst-fill flow control: the MCU fills the FIFO with batched WRITE_SAME
    // bursts (one held-low CS transaction per chunk) to keep up with the 1MHz
    // CPU drain. 'tx_room' tells the MCU there is space for a whole chunk, so
    // it can push IEEE_TX_BURST_CHUNK bytes without checking full per byte and
    // without risking an overflow (over-full writes are silently dropped).
    wire tx_room = tx_count <= ($clog2(TX_DEPTH)+1)'(TX_DEPTH - IEEE_TX_BURST_CHUNK);

    // Separate FIFO for the command/status channel (sa 15), so status reads
    // can never be polluted by queued file data and vice versa.
    localparam TXS_DEPTH = 32;
    logic [8:0] txs_mem [TXS_DEPTH-1:0];
    logic [$clog2(TXS_DEPTH)-1:0] txs_wr = '0, txs_rd = '0;
    logic [$clog2(TXS_DEPTH):0]   txs_count = '0;
    wire txs_empty = txs_count == 0;
    wire txs_full  = txs_count == ($clog2(TXS_DEPTH)+1)'(TXS_DEPTH);

    // Talker source: status FIFO on channel 15, data FIFO otherwise.
    wire serving_status = talk_sa[3:0] == 4'hF;

    // ------------------------------------------------------------------
    // Device protocol state
    // ------------------------------------------------------------------
    // The emulation answers four unit addresses, DEV_ADDR through DEV_ADDR+3
    // (devices 8 through 11), each a dual-drive unit. Handshake and FIFOs
    // are shared, but roles and secondary selections belong to each unit.
    // The MCU also learns the unit from commands forwarded through RX.
    // DEV_ADDR must be divisible by four so the group is a single mask match.
    localparam logic [4:0] DEV_ADDR = 5'd8;
    localparam int UNIT_COUNT = 4;
    wire [1:0] primary_unit = ~cpu_dio[1:0];
    wire own_address = (~cpu_dio & 8'h1C) == {3'b0, DEV_ADDR[4:2], 2'b00}
        && unit_mask[primary_unit];

    logic [UNIT_COUNT-1:0] listen_units = '0;
    wire listening = |(listen_units & unit_mask);
    logic talking   = 1'b0;
    logic [7:0] sa = 8'h00;
    logic [UNIT_COUNT-1:0][7:0] unit_sa = '0;
    logic addressed = 1'b0;
    logic [1:0] addressed_unit = '0;
    logic [1:0] talk_unit = '0;
    wire [7:0] talk_sa = unit_sa[talk_unit];
    wire owner_matches = !(|(tx_owner & IEEE_TX_OWNER_PAUSE_MASK))
        && (!(|(tx_owner & IEEE_TX_OWNER_VALID_MASK))
            || (tx_owner[IEEE_TX_OWNER_UNIT_SHIFT +: 2] == talk_unit
                && tx_owner[3:0] == talk_sa[3:0]));

    wire atn_active = enable && !cpu_atn_n;

    logic prev_bus_dav_n = 1'b1;
    logic prev_atn_n     = 1'b1;
    logic atn_end_pending = 1'b0;
    wire atn_end_due = atn_end_pending || (!prev_atn_n && cpu_atn_n);
    logic listen_checkpoint_armed = 1'b0;
    logic [3:0] listen_byte_units = '0;
    logic [3:0] listen_end_pending = '0;
    wire listen_end_due = |listen_end_pending;

    // Native TLK20/TLK30 prefetches on NRFD before TLK35 waits for NDAC.
    typedef enum logic [2:0] { T_IDLE, T_DAV, T_PREFETCH, T_ACK, T_REARM, T_DONE } talk_state_t;
    talk_state_t talk_st = T_IDLE;
    logic talk_eoi_fetched = 1'b0;
    logic talk_event_pending = 1'b0;
    logic [7:0] talk_event_data = '0;
    wire talk_event_due = talk_event_pending || (atn_active && talk_eoi_fetched);

    // RX push request from the acceptor
    logic rx_push = 1'b0;          // init: else a phantom push under rand reset
    logic [11:0] rx_push_data = '0;


    // Data-FIFO flush, requested by the MCU (on OPEN/CLOSE) via CTRL bit 2.
    // UNTALK deliberately flushes nothing: the kernel reads files in several
    // TALK/UNTALK passes and expects the channel to continue where it left
    // off, exactly like a real drive.
    logic tx_flush;
    logic txs_flush;

    // TX pop bookkeeping (routed to the FIFO being served)
    logic tx_pop;
    logic txs_pop;

    always_ff @(posedge wb_clock_i) begin
        rx_push <= 1'b0;
        tx_pop  <= 1'b0;
        txs_pop <= 1'b0;
        tx_flush <= 1'b0;
        txs_flush <= 1'b0;

        if (ifc_i) begin
            tx_prefetched <= '0;
            txs_prefetched <= '0;
            atn_end_pending <= 1'b0;
            listen_checkpoint_armed <= 1'b0;
            listen_byte_units <= '0;
            listen_end_pending <= '0;
            talk_event_pending <= 1'b0;
            talk_eoi_fetched <= 1'b0;
            talk_event_data <= '0;
            // Supported PET/fabric resets are MCU-coordinated with ieee_drive_reset().
            // Independently pulling the physical CPU reset pin low is unsupported.
            listen_units   <= '0;
            talking        <= 1'b0;
            sa             <= 8'h00;
            unit_sa        <= '0;
            addressed      <= 1'b0;
            addressed_unit <= '0;
            talk_unit      <= '0;
            talk_st        <= T_IDLE;
            dev_nrfd_n     <= 1'b1;
            dev_ndac_n     <= 1'b1;
            dev_dav_n      <= 1'b1;
            dev_eoi_n      <= 1'b1;
            dev_dio        <= 8'hFF;
            prev_bus_dav_n <= 1'b1;
            prev_atn_n     <= 1'b1;
            rx_push_data   <= '0;
        end else if (flush || !enable) begin
            if (flush) tx_prefetched <= '0;
            if (flush) txs_prefetched <= '0;
            atn_end_pending <= 1'b0;
            listen_checkpoint_armed <= 1'b0;
            listen_byte_units <= '0;
            listen_end_pending <= '0;
            talk_event_pending <= 1'b0;
            talk_eoi_fetched <= 1'b0;
            talk_event_data <= '0;
            listen_units   <= '0;
            talking        <= 1'b0;
            sa             <= 8'h00;
            unit_sa        <= '0;
            addressed      <= 1'b0;
            addressed_unit <= '0;
            talk_unit      <= '0;
            talk_st        <= T_IDLE;
            dev_nrfd_n     <= 1'b1;
            dev_ndac_n     <= 1'b1;
            dev_dav_n      <= 1'b1;
            dev_eoi_n      <= 1'b1;
            dev_dio        <= 8'hFF;
            prev_bus_dav_n <= 1'b1;
            prev_atn_n     <= 1'b1;
        end else begin
            // Withdraw old roles before an accepted command selects a retained
            // peer. Later per-bit/role updates must win over these defaults.
            if (|(listen_units & ~unit_mask)) listen_units <= listen_units & unit_mask;
            if (addressed && !unit_mask[addressed_unit]) addressed <= 1'b0;
            if (talking && !unit_mask[talk_unit]) talking <= 1'b0;

            // Retain source progress before later ATN bytes and release boundaries.
            if (atn_end_due) atn_end_pending <= 1'b1;
            if (atn_active) listen_checkpoint_armed <= 1'b1;
            if (talk_event_pending) begin
                if (rx_room) begin
                    rx_push <= 1'b1;
                    rx_push_data <= {1'b1, 3'b0, talk_event_data};
                    talk_event_pending <= 1'b0;
                end
            end else if (listen_end_due && !talk_event_due && rx_room) begin
                rx_push <= 1'b1;
                rx_push_data <= {2'b01, 2'b0, 4'b0, listen_end_pending};
                listen_end_pending <= '0;
            end else if (atn_end_due && !talk_event_due && rx_room) begin
                rx_push <= 1'b1;
                rx_push_data <= {2'b01, 10'b0};
                atn_end_pending <= 1'b0;
            end
            if (atn_active || listening) begin
                if (prev_atn_n && !cpu_atn_n) begin
                    dev_ndac_n <= 1'b0;    // join the handshake on ATN assert
                end

                if (prev_bus_dav_n && !bus_dav_n && rx_room
                    && !atn_end_due && !talk_event_due && !listen_end_due) begin
                    // Byte valid and room to store it: capture and accept.
                    // (While RX is full, hold NDAC asserted and wait -- IEEE
                    // flow control backpressures the CPU until the MCU drains
                    // a byte, instead of ACKing a command we silently drop.)
                    if (atn_active) begin
                        // Command byte: update protocol state and forward to MCU.
                        priority casez (~cpu_dio)
                            8'h3F: begin                                     // UNLISTEN
                                listen_units <= '0;
                                addressed <= 1'b0;
                            end
                            8'h5F: begin                                     // UNTALK (channel data persists)
                                talking <= 1'b0;
                                addressed <= 1'b0;
                            end
                            8'b001?_????: begin
                                addressed <= own_address;
                                if (own_address) begin
                                    addressed_unit <= primary_unit;
                                    listen_units[primary_unit] <= 1'b1;
                                    unit_sa[primary_unit] <= 8'h20;
                                    if (talk_unit == primary_unit) talking <= 1'b0;
                                end
                            end
                            8'b010?_????: begin
                                addressed <= own_address;
                                talking <= own_address;
                                if (own_address) begin
                                    addressed_unit <= primary_unit;
                                    listen_units[primary_unit] <= 1'b0;
                                    unit_sa[primary_unit] <= 8'h20;
                                    talk_unit <= primary_unit;
                                end
                            end
                            8'b011?_????: if (addressed && unit_mask[addressed_unit]) begin
                                sa <= ~cpu_dio;                               // secondary (data)
                                unit_sa[addressed_unit] <= ~cpu_dio;
                                // Fresh status per request: the kernel re-reads
                                // channel 15 often; stale unread status bytes
                                // must not misalign the next read.
                                if (talking && talk_unit == addressed_unit
                                    && (~cpu_dio & 8'h0F) == 8'h0F)
                                    txs_flush <= 1'b1;
                            end
                            8'b1110_????: if (addressed && unit_mask[addressed_unit]
                                && listen_units[addressed_unit]) begin
                                sa <= ~cpu_dio;                               // CLOSE
                                unit_sa[addressed_unit] <= ~cpu_dio;
                                // Discard queued file data in fabric, before
                                // the MCU even sees the command -- but only
                                // for real data channels. Firmware handles
                                // ch15 OPEN/CLOSE lifetimes without discarding
                                // an unrelated unit's selected response.
                                if ((~cpu_dio & 8'h0F) != 8'h0F)
                                    tx_flush <= 1'b1;
                            end
                            8'b1111_????: if (addressed && unit_mask[addressed_unit]
                                && listen_units[addressed_unit]) begin
                                sa <= ~cpu_dio;                               // OPEN
                                unit_sa[addressed_unit] <= ~cpu_dio;
                                if ((~cpu_dio & 8'h0F) != 8'h0F)
                                    tx_flush <= 1'b1;
                            end
                            default: ;
                        endcase
                        rx_push <= 1'b1;
                        rx_push_data <= {2'b0, !cpu_eoi_n, 1'b1, ~cpu_dio};
                    end else begin
                        rx_push <= 1'b1;
                        rx_push_data <= {2'b0, !bus_eoi_n, 1'b0, ~bus_dio};
                    end
                    dev_nrfd_n <= 1'b0;
                    dev_ndac_n <= 1'b1;    // accepted
                end else if (!prev_bus_dav_n && bus_dav_n) begin
                    // DAV released: re-arm for the next byte.
                    dev_ndac_n <= 1'b0;
                    dev_nrfd_n <= 1'b1;
                end
            end else begin
                dev_ndac_n <= 1'b1;
                dev_nrfd_n <= 1'b1;
            end
            // LSN21 observes DAV even if transport is full and the sender times out.
            if (!atn_active && listening && listen_checkpoint_armed && !bus_dav_n) begin
                listen_byte_units <= listen_units & unit_mask;
                listen_checkpoint_armed <= 1'b0;
            end
            // ATNIRQ abandons LSN21, but cannot retract an already completed exit.
            if (atn_active) listen_byte_units <= '0;
            else if (|listen_byte_units && bus_dav_n) begin
                listen_end_pending <= listen_byte_units;
                listen_byte_units <= '0;
            end

            // A peer virtual unit can accept while the selected source talks.
            if (atn_active) begin
                // ATN aborts presentation without repeating an earlier GET.
                if (talk_eoi_fetched) begin
                    talk_event_pending <= 1'b1;
                    talk_event_data <= IEEE_RX_TALK_EOI_GET_MASK
                        | {2'b0, talk_unit, talk_sa[3:0]};
                    talk_eoi_fetched <= 1'b0;
                end
                talk_st   <= T_IDLE;
                dev_dav_n <= 1'b1;
                dev_dio   <= 8'hFF;
                dev_eoi_n <= 1'b1;
            end else if (talking) begin
                // Talker role (source handshake). Bytes come from the TX FIFO;
                // if it runs dry mid-file the handshake simply pauses (the
                // CPU waits on DAV) until the MCU refills it.
                unique case (talk_st)
                    T_IDLE: begin
                        if (!talk_event_due && !listen_end_due && owner_matches
                            && (serving_status ? !txs_empty : !tx_empty))
                            talk_st <= T_DAV;
                    end
                    // Source handshake may only begin once an acceptor is
                    // present (NDAC asserted) and ready (NRFD released) --
                    // starting on NRFD alone loses the first byte if the
                    // acceptor hasn't configured NDAC yet. A virtual peer
                    // also needs RX space before DAV, or active-byte protection
                    // prevents firmware from draining the queue it must accept into.
                    T_DAV: if (bus_nrfd_n && !bus_ndac_n
                               && !talk_event_due && !atn_end_due && !listen_end_due
                               && (!listening || (rx_room && !atn_end_due))
                               && owner_matches && (serving_status ? !txs_empty : !tx_empty)) begin
                        dev_dio   <= serving_status ? ~txs_mem[txs_rd][7:0] : ~tx_mem[tx_rd][7:0];
                        dev_eoi_n <= serving_status ? ~txs_mem[txs_rd][8]   : ~tx_mem[tx_rd][8];
                        dev_dav_n <= 1'b0;
                        talk_st   <= T_PREFETCH;
                    end
                    T_PREFETCH, T_ACK: begin
                        // Owner changes stop new presentations, not this one.
                        // Native TLK35 holds DAV/DIO/EOI until NDAC releases.
                        if (talk_st == T_PREFETCH && !bus_nrfd_n) begin
                            if (serving_status) begin
                                txs_pop <= 1'b1;
                                txs_prefetched <= txs_prefetched + 1'b1;
                            end
                            else begin
                                tx_pop <= 1'b1;
                                // Count GET itself, even if a simultaneous
                                // queue discard wins the FIFO pointer update.
                                tx_prefetched <= tx_prefetched + 1'b1;
                                if (!dev_eoi_n) talk_eoi_fetched <= 1'b1;
                            end
                            talk_st <= T_ACK;
                        end
                        // Both phases can finish on NDAC, including unread
                        // termination or NRFD and NDAC observed together.
                        if (bus_ndac_n) begin
                            dev_dav_n <= 1'b1;
                            dev_dio   <= 8'hFF;
                            dev_eoi_n <= 1'b1;
                            if (talk_st == T_PREFETCH && bus_nrfd_n) begin
                                talk_event_pending <= 1'b1;
                                talk_event_data <= {2'b00, talk_unit, talk_sa[3:0]};
                                talk_st <= T_DONE;
                            end else begin
                                talk_st <= T_REARM;
                            end
                        end
                    end
                    T_REARM: if (!bus_ndac_n) begin     // all acceptors re-armed
                        if (talk_eoi_fetched) begin
                            talk_event_pending <= 1'b1;
                            talk_event_data <= IEEE_RX_TALK_EOI_GET_MASK | IEEE_RX_TALK_RECHECK_MASK
                                | {2'b0, talk_unit, talk_sa[3:0]};
                            talk_eoi_fetched <= 1'b0;
                        end
                        talk_st <= T_IDLE;
                    end
                    T_DONE: ;
                endcase
            end else begin
                dev_dav_n  <= 1'b1;
                dev_eoi_n  <= 1'b1;
                dev_dio    <= 8'hFF;
                talk_st    <= T_IDLE;
            end

            // Edge-retention: if the DAV falling edge landed while the RX
            // FIFO was full, the capture above was suppressed -- keep
            // prev_bus_dav_n high so the edge re-fires once the MCU drains
            // a slot, instead of being lost forever (bus deadlock).
            if (!(prev_bus_dav_n && !bus_dav_n
                && (!rx_room || atn_end_due || talk_event_due || listen_end_due)
                && (atn_active || listening)))
                prev_bus_dav_n <= bus_dav_n;
            prev_atn_n     <= cpu_atn_n;
            // A host media change may remove one unit without resetting peers.
            if (talking && !unit_mask[talk_unit]) begin
                talk_st <= T_IDLE;
                talk_eoi_fetched <= 1'b0;
                dev_dav_n <= 1'b1;
                dev_eoi_n <= 1'b1;
                dev_dio <= 8'hFF;
            end
        end
    end

    // ------------------------------------------------------------------
    // Wishbone interface + FIFO storage management
    // ------------------------------------------------------------------
    wire [IEEE_REG_ADDR_WIDTH-1:0] reg_addr = wbp_addr_i[IEEE_REG_ADDR_WIDTH-1:0];
    wire wb_req = wbp_sel_i && wbp_cycle_i && wbp_strobe_i;

    assign wbp_stall_o = 1'b0;

    // Talker starved with the CPU waiting. Checks only
    // the FIFO the active channel serves -- the data FIFO is legitimately
    // empty during channel-15 status phases.
    wire talk_starved = talking && talk_st == T_IDLE
                        && (serving_status ? txs_empty : tx_empty)
                        && !cpu_ndac_n && bus_nrfd_n;  // acceptor actually waiting

    wire [DATA_WIDTH-1:0] status;
    assign status[IEEE_ST_TALK_STARVED_BIT] = talk_starved;
    assign status[IEEE_ST_TALKING_BIT] = talking;
    assign status[IEEE_ST_LISTENING_BIT] = listening;
    assign status[IEEE_ST_ATN_BIT] = atn_active;
    assign status[IEEE_ST_TX_EMPTY_BIT] = tx_empty;
    assign status[IEEE_ST_TX_FULL_BIT] = tx_full;
    assign status[IEEE_ST_RX_ATN_BIT] = rx_mem[rx_rd][8];
    assign status[IEEE_ST_RX_AVAIL_BIT] = !rx_empty;

    logic rx_pop_req = 1'b0, tx_push_req = 1'b0, txs_push_req = 1'b0;
    logic mcu_tx_flush, mcu_txs_flush;
    logic [8:0] tx_push_data = '0;
    logic [8:0] txs_push_data = '0;

    always_ff @(posedge wb_clock_i) begin
        wbp_ack_o <= 1'b0;
        flush <= 1'b0;
        mcu_tx_flush <= 1'b0;
        mcu_txs_flush <= 1'b0;
        rx_pop_req <= 1'b0;
        tx_push_req <= 1'b0;
        txs_push_req <= 1'b0;

        if (ifc_i) begin
            tx_push_data <= '0;
            txs_push_data <= '0;
            tx_owner <= '0;
        end else if (flush) begin
            tx_owner <= '0;
        end

        // IFC resets the emulated drive, but the MCU control plane remains
        // available so firmware can configure the drive while PET RES is held.
        if (wb_req) begin
            wbp_ack_o <= 1'b1;
            unique case (reg_addr)
                IEEE_REG_CTRL: begin
                    // FIFO-only flushes preserve GET counters and protocol roles.
                    wbp_data_o <= (tx_room ? IEEE_CTRL_RD_TX_ROOM_MASK : DATA_WIDTH'(0))
                                | (enable ? IEEE_CTRL_RD_ENABLE_MASK : DATA_WIDTH'(0))
                                | (!serving_status && !dev_dav_n
                                    ? IEEE_CTRL_RD_DATA_ACTIVE_MASK : DATA_WIDTH'(0))
                                | (!rx_empty && rx_mem[rx_rd][9]
                                    ? IEEE_CTRL_RD_RX_EOI_MASK : DATA_WIDTH'(0))
                                | (!rx_empty && rx_mem[rx_rd][10]
                                    ? IEEE_CTRL_RD_RX_ATN_END_MASK : DATA_WIDTH'(0))
                                | (serving_status && !dev_dav_n
                                    ? IEEE_CTRL_RD_STATUS_ACTIVE_MASK : DATA_WIDTH'(0))
                                | (!rx_empty && rx_mem[rx_rd][11]
                                    ? IEEE_CTRL_RD_RX_TALK_EVENT_MASK : DATA_WIDTH'(0));
                    if (wbp_we_i) begin
                        flush        <= wbp_data_i[IEEE_CTRL_FLUSH_BIT];
                        mcu_tx_flush <= wbp_data_i[IEEE_CTRL_DATA_FLUSH_BIT];
                        mcu_txs_flush <= wbp_data_i[IEEE_CTRL_STATUS_FLUSH_BIT];
                        unit_mask <= wbp_data_i[IEEE_CTRL_UNIT_MASK_SHIFT +: $bits(unit_mask)];
                    end
                end
                IEEE_REG_STATUS: wbp_data_o <= status;
                // NOTE: the SPI bridge's pipelined read protocol prefetches
                // address+1 after every read, so register READS must be free
                // of side effects. Reading RX returns the head byte; popping
                // it requires an explicit WRITE to IEEE_REG_RX.
                IEEE_REG_RX: begin
                    wbp_data_o <= rx_mem[rx_rd][7:0];
                    if (wbp_we_i && !ifc_i && !rx_empty) rx_pop_req <= 1'b1;
                end
                IEEE_REG_TX: begin
                    wbp_data_o <= tx_prefetched;
                    if (wbp_we_i && !ifc_i && !tx_full) begin
                        tx_push_req  <= 1'b1;
                        tx_push_data <= {1'b0, wbp_data_i};
                    end
                end
                IEEE_REG_TX_LAST: begin
                    wbp_data_o <= 8'h00;   // write-only (data TX FIFO push, EOI)
                    if (wbp_we_i && !ifc_i && !tx_full) begin
                        tx_push_req  <= 1'b1;
                        tx_push_data <= {1'b1, wbp_data_i};
                    end
                end
                IEEE_REG_SA: begin
                    wbp_data_o <= sa;
                    if (wbp_we_i && !ifc_i && !flush) tx_owner <= wbp_data_i;
                end
                IEEE_REG_TXS: begin
                    wbp_data_o <= txs_prefetched;
                    if (wbp_we_i && !ifc_i && !txs_full) begin
                        txs_push_req  <= 1'b1;
                        txs_push_data <= {1'b0, wbp_data_i};
                    end
                end
                IEEE_REG_TXS_LAST: begin
                    wbp_data_o <= 8'h00;   // write-only (status TX FIFO push, EOI)
                    if (wbp_we_i && !ifc_i && !txs_full) begin
                        txs_push_req  <= 1'b1;
                        txs_push_data <= {1'b1, wbp_data_i};
                    end
                end
                // Expose snooped guest PIA2/VIA registers (masking PIA IRQ flag bits 7:6) for unmount restoration.
                IEEE_REG_PIA2_ORA:  wbp_data_o <= pia2_ora;
                IEEE_REG_PIA2_DDRA: wbp_data_o <= pia2_ddra;
                IEEE_REG_PIA2_CRA:  wbp_data_o <= {2'b00, pia2_cra[5:0]};
                IEEE_REG_PIA2_ORB:  wbp_data_o <= pia2_pb_out;
                IEEE_REG_PIA2_DDRB: wbp_data_o <= pia2_ddrb;
                IEEE_REG_PIA2_CRB:  wbp_data_o <= {2'b00, pia2_crb[5:0]};
                IEEE_REG_VIA_ORB:   wbp_data_o <= via_orb;
                IEEE_REG_VIA_DDRB:  wbp_data_o <= via_ddrb;
                default: wbp_data_o <= 8'h00;
            endcase
        end
    end

    // FIFO pointer/storage updates (single writer per FIFO side).
    always_ff @(posedge wb_clock_i) begin
        if (ifc_i || flush) begin
            rx_wr <= '0; rx_rd <= '0; rx_count <= '0;
            tx_wr <= '0; tx_rd <= '0; tx_count <= '0;
            txs_wr <= '0; txs_rd <= '0; txs_count <= '0;
        end else begin
            if (tx_flush || mcu_tx_flush) begin
                tx_wr <= '0; tx_rd <= '0; tx_count <= '0;
            end else begin
                if (tx_push_req && !tx_full) begin
                    tx_mem[tx_wr] <= tx_push_data;
                    tx_wr <= tx_wr + 1'b1;
                end
                if (tx_pop && !tx_empty) tx_rd <= tx_rd + 1'b1;
                case ({tx_push_req && !tx_full, tx_pop && !tx_empty})
                    2'b10: tx_count <= tx_count + 1'b1;
                    2'b01: tx_count <= tx_count - 1'b1;
                    default: ;
                endcase
            end

            if (txs_flush || mcu_txs_flush) begin
                txs_wr <= '0; txs_rd <= '0; txs_count <= '0;
            end else begin
                if (txs_push_req && !txs_full) begin
                    txs_mem[txs_wr] <= txs_push_data;
                    txs_wr <= txs_wr + 1'b1;
                end
                if (txs_pop && !txs_empty) txs_rd <= txs_rd + 1'b1;
                case ({txs_push_req && !txs_full, txs_pop && !txs_empty})
                    2'b10: txs_count <= txs_count + 1'b1;
                    2'b01: txs_count <= txs_count - 1'b1;
                    default: ;
                endcase
            end

            if (rx_push && !rx_full) begin
                rx_mem[rx_wr] <= rx_push_data;
                rx_wr <= rx_wr + 1'b1;
            end
            if (rx_pop_req && !rx_empty) rx_rd <= rx_rd + 1'b1;
            case ({rx_push && !rx_full, rx_pop_req && !rx_empty})
                2'b10: rx_count <= rx_count + 1'b1;
                2'b01: rx_count <= rx_count - 1'b1;
                default: ;
            endcase
        end
    end

    // ------------------------------------------------------------------
    // CPU read injection
    // ------------------------------------------------------------------
    wire [7:0] via_pb_composed = {
        bus_dav_n,                              // PB7: DAV in
        bus_nrfd_n,                             // PB6: NRFD in
        !vert_i,                                // PB5: vertical retrace
        via_ddrb[4] ? via_orb[4] : 1'b1,        // PB4: cassette #2 motor (readback)
        via_ddrb[3] ? via_orb[3] : 1'b1,        // PB3: cassette write (readback)
        cpu_atn_n,                              // PB2: ATN out (readback)
        cpu_nrfd_n,                             // PB1: buffer input, not the shared NRFD wire
        bus_ndac_n                              // PB0: NDAC in
    };

    wire [7:0] pia1_pa_composed = {
        diag_i,             // PA7: diagnostic sense (physical pin)
        bus_eoi_n,          // PA6: EOI in
        cassette_sense,     // PA5/PA4: last physical cassette switch observation
        pia1_pa_out         // PA3-0: keyboard column select (readback)
    };

    // Compose PIA2 Port A readback from output-configured ORA bits and input-configured virtual DIO bits.
    wire [7:0] pia2_pa_composed = (pia2_ora & pia2_ddra) | (bus_dio & ~pia2_ddra);

    // Register per-chip external chip-select claims on both cpu_addr_strobe_i
    // (one clock before rising PHI2, matching typical tBVD setup) and snap_dly[0]
    // (at rising PHI2, covering worst-case 30 ns W65C02S tBVD plus pad/fabric
    // routing alongside address_decoding.sv), then hold steady across PHI2 and
    // any late-BE address tail:
    //   - PIA2 claims every cycle ($E820..$E823) so DDR/control/latch writes stay off physical PIA2.
    //   - VIA claims $E842 (DDRB read/write) and $E840 reads, letting $E840 writes reach physical cassette I/O.
    //   - PIA1 claims $E810 ORA reads only while DAV or EOI is low, leaving idle reads on physical PIA1.
    wire [PIA_RS_WIDTH-1:0] addr_pia_rs = rs_i[PIA_RS_WIDTH-1:0];
    wire pia1_read_claim = pia1_cra[2] && addr_pia_rs == 2'd0 && (!bus_dav_n || !bus_eoi_n);
    wire via_claim_reg   = rs_i == 4'd2 || (!cpu_we_i && rs_i == 4'd0);

    always_ff @(posedge wb_clock_i) begin
        if (ifc_i || !cpu_be_i || !enable) begin
            claim_pia1_o <= 1'b0;
            claim_pia2_o <= 1'b0;
            claim_via_o  <= 1'b0;
        end else if (cpu_addr_strobe_i || snap_dly[0]) begin
            claim_pia1_o <= !cpu_we_i && pia1_read_claim;
            claim_pia2_o <= 1'b1;
            claim_via_o  <= via_claim_reg;
        end
    end

    always_ff @(posedge wb_clock_i) begin
        if (ifc_i) begin
            cpu_data_o  <= 8'hFF;
            cpu_data_oe <= 1'b0;
        end else begin
            cpu_data_oe <= 1'b0;

            if (cpu_addr_strobe_i) cpu_data_oe <= 1'b0;
            if (enable && cpu_be_i && snap_valid && !snap_we) begin
                if (snap_pia2 && claim_pia2_o) begin
                    // Serve all four PIA2 addresses from the internal virtual-bus and shadow model.
                    unique case (pia_rs)
                        2'd0: cpu_data_o <= pia2_cra[2] ? pia2_pa_composed : pia2_ddra;
                        2'd1: cpu_data_o <= {2'b00, pia2_cra[5:0]};
                        2'd2: cpu_data_o <= pia2_crb[2] ? cpu_dio : pia2_ddrb;
                        2'd3: cpu_data_o <= {2'b00, pia2_crb[5:0]};
                        default: ;
                    endcase
                    cpu_data_oe <= 1'b1;
                end else if (snap_via && claim_via_o) begin
                    // Serve composed VIA Port B ($E840) or snooped VIA DDRB ($E842).
                    cpu_data_o  <= (snap_rs == 4'd0) ? via_pb_composed : via_ddrb;
                    cpu_data_oe <= 1'b1;
                end else if (snap_pia1 && claim_pia1_o) begin
                    // Serve composed PIA1 Port A ($E810) while DAV or EOI is asserted.
                    cpu_data_o  <= pia1_pa_composed;
                    cpu_data_oe <= 1'b1;
                end
            end
        end
    end
endmodule
