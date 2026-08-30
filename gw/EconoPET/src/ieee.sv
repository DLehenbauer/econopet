// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// IEEE-488 disk-drive emulation (devices 8 and 9).
//
// Sits between the soft CPU and the physical PIA2/VIA the same way
// keyboard.sv sits in front of PIA1: it SNOOPS the CPU's writes to the
// IEEE-related I/O registers and, when enabled, INJECTS the CPU's reads of
// the IEEE status/input registers so that an emulated drive appears on the
// otherwise-empty bus. Disabled it is fully passive (physical devices are
// untouched); enabled it replaces the CPU's view of the bus, so ALL physical
// IEEE devices are masked -- enable only with nothing on the connector. The byte-level handshake (acceptor and talker roles)
// runs here in fabric -- IEEE-488 is flow-controlled, so the MCU can
// service the FIFOs over Wishbone/SPI at its own rate while the CPU
// blocks in its handshake loops.
//
// PET IEEE-488 register map (per the Waterloo kernel):
//   PIA2 $E820 PA  = DIO in            (inject while our talker drives data)
//   PIA2 $E821 CRA = CA2 -> NDAC out   (snoop: bits[5:4]==11 -> level=bit3)
//   PIA2 $E822 PB  = DIO out           (snoop; complement = byte value)
//   PIA2 $E823 CRB = CB2 -> DAV out    (snoop)
//   VIA  $E840 PB  = bit0 NDAC in, bit1 NRFD out, bit2 ATN out,
//                    bit6 NRFD in, bit7 DAV in   (inject full composed byte)
//   PIA1 $E810 PA  = bit6 EOI in       (inject only while asserting EOI)
//
// The Waterloo load sequence this must serve:
//   LISTEN 8, OPEN ch ($Fx), name "1:<file>,PRG", UNLISTEN,
//   TALK 8 ch15 -> CR-terminated status string, UNTALK,
//   TALK 8 ch0  -> file data (EOI on last byte), UNTALK,
//   TALK ch15 status re-check, CLOSE, final status read.
module ieee (
    input  logic wb_clock_i,

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

    // CPU bus snoop/inject (soft 6809 side)
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

    input  logic                     diag_i,        // PIA1 PA7 (physical pin)
    input  logic                     vert_i         // vertical drive, for VIA PB5
);
    // ------------------------------------------------------------------
    // Control / status registers
    // ------------------------------------------------------------------
    logic enable = 1'b0;
    logic flush = 1'b0;

    // ------------------------------------------------------------------
    // Snooped CPU-side IEEE line states (all _n: 1 = released)
    // ------------------------------------------------------------------
    logic [7:0] pia2_pb_out = 8'hFF;   // DIO out register (active-low)
    logic [7:0] pia2_cra    = 8'h00;
    logic [7:0] pia2_crb    = 8'h00;
    logic [7:0] via_orb     = 8'hFF;
    logic [3:0] pia1_pa_out = 4'h0;    // keyboard column select (for $E810 compose)

    wire cpu_ndac_n = (pia2_cra[5:4] == 2'b11) ? pia2_cra[3] : 1'b1;
    wire cpu_dav_n  = (pia2_crb[5:4] == 2'b11) ? pia2_crb[3] : 1'b1;
    wire cpu_atn_n  = via_orb[2];
    wire cpu_nrfd_n = via_orb[1];

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
        snap_dly <= {snap_dly[1:0], cpu_addr_strobe_i};
        // BE fall ends the cycle -- a stale snapshot must not inject into the next.
        if (cpu_addr_strobe_i || !cpu_be_i) snap_valid <= 1'b0;
        if (snap_dly[2]) begin
            snap_valid <= 1'b1;
            snap_pia1 <= pia1_cs_i;
            snap_pia2 <= pia2_cs_i;
            snap_via  <= via_cs_i;
            snap_rs   <= rs_i;
            snap_we   <= cpu_we_i;
        end
    end

    wire [PIA_RS_WIDTH-1:0] pia_rs = snap_rs[PIA_RS_WIDTH-1:0];
    wire cpu_wr = cpu_data_strobe_i && snap_we;

    always_ff @(posedge wb_clock_i) begin
        if (cpu_wr) begin
            if (snap_pia2) begin
                unique case (pia_rs)
                    2'd1: pia2_cra <= cpu_data_i;
                    2'd2: if (pia2_crb[2]) pia2_pb_out <= cpu_data_i;  // else DDRB
                    2'd3: pia2_crb <= cpu_data_i;
                    default: ;
                endcase
            end
            if (snap_via && snap_rs == 4'd0) via_orb <= cpu_data_i;
            if (snap_pia1 && pia_rs == 2'd0) pia1_pa_out <= cpu_data_i[3:0];
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
    wire [7:0] bus_dio = pia2_pb_out & dev_dio;

    // ------------------------------------------------------------------
    // FIFOs
    //   RX: CPU -> MCU, 9-bit entries {atn, data} (commands tagged)
    //   TX: MCU -> CPU, 9-bit entries {eoi, data}
    // ------------------------------------------------------------------
    localparam RX_DEPTH = 32;
    // Must hold more than one firmware main-loop period of drained bytes or
    // the kernel's counted read underruns mid-record; 1024 covers a loop
    // period at the 1MHz drain rate. The combinational read port maps this
    // to LUT-RAM, so depth is LUT-bounded.
    localparam TX_DEPTH = 1024;

    logic [8:0] rx_mem [RX_DEPTH-1:0];
    logic [$clog2(RX_DEPTH)-1:0] rx_wr = '0, rx_rd = '0;
    logic [$clog2(RX_DEPTH):0]   rx_count = '0;
    wire rx_empty = rx_count == 0;
    wire rx_full  = rx_count == ($clog2(RX_DEPTH)+1)'(RX_DEPTH);

    logic [8:0] tx_mem [TX_DEPTH-1:0];
    logic [$clog2(TX_DEPTH)-1:0] tx_wr = '0, tx_rd = '0;
    logic [$clog2(TX_DEPTH):0]   tx_count = '0;
    wire tx_empty = tx_count == 0;
    wire tx_full  = tx_count == ($clog2(TX_DEPTH)+1)'(TX_DEPTH);

    // Burst-fill flow control: the MCU fills the FIFO with batched WRITE_SAME
    // bursts (one held-low CS transaction per chunk) to keep up with the 1MHz
    // CPU drain. 'tx_room' tells the MCU there is space for a whole chunk, so
    // it can push TX_BURST_CHUNK bytes without checking full per byte and
    // without risking an overflow (over-full writes are silently dropped).
    // The firmware DOS layer (follow-up PR) must match TX_BURST_CHUNK = 64..
    localparam int unsigned TX_BURST_CHUNK = 64;
    wire tx_room = tx_count <= ($clog2(TX_DEPTH)+1)'(TX_DEPTH - TX_BURST_CHUNK);

    // Separate FIFO for the command/status channel (sa 15), so status reads
    // can never be polluted by queued file data and vice versa.
    localparam TXS_DEPTH = 32;
    logic [8:0] txs_mem [TXS_DEPTH-1:0];
    logic [$clog2(TXS_DEPTH)-1:0] txs_wr = '0, txs_rd = '0;
    logic [$clog2(TXS_DEPTH):0]   txs_count = '0;
    wire txs_empty = txs_count == 0;
    wire txs_full  = txs_count == ($clog2(TXS_DEPTH)+1)'(TXS_DEPTH);

    // Talker source: status FIFO on channel 15, data FIFO otherwise.
    wire serving_status = sa[3:0] == 4'hF;

    // ------------------------------------------------------------------
    // Device protocol state
    // ------------------------------------------------------------------
    // The emulation answers TWO unit addresses, DEV_ADDR and DEV_ADDR+1
    // (devices 8 and 9) -- each a dual-drive unit, so Super-OS/9's d8d9
    // configuration sees four drives. The fabric doesn't care WHICH unit is
    // addressed: handshake state, sa, and the FIFOs are shared (only one
    // talker/listener is ever active at a time), and the MCU learns the unit
    // from the raw LISTEN/TALK command bytes forwarded through the RX FIFO.
    // DEV_ADDR must stay even so the pair is a single address-mask match.
    localparam logic [4:0] DEV_ADDR = 5'd8;

    logic listening = 1'b0;
    logic talking   = 1'b0;
    logic [7:0] sa = 8'h00;

    wire atn_active = enable && !cpu_atn_n;

    logic prev_bus_dav_n = 1'b1;
    logic prev_atn_n     = 1'b1;

    // Talker sub-state
    typedef enum logic [1:0] { T_IDLE, T_DAV, T_ACK, T_REARM } talk_state_t;
    talk_state_t talk_st = T_IDLE;

    // Set once the CPU has read the in-flight byte at $E820. The kernel
    // re-arms NDAC after every accepted byte, including the last of a
    // counted read, so a terminate is line-identical to an accept. Pop only
    // consumed bytes: the phantom accept then leaves the byte queued for
    // the next TALK.
    logic byte_consumed = 1'b0;

    // RX push request from the acceptor
    logic rx_push = 1'b0;          // init: else a phantom push under rand reset
    logic [8:0] rx_push_data = '0;


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

        if (flush || !enable) begin
            listening <= 1'b0;
            talking   <= 1'b0;
            talk_st   <= T_IDLE;
            dev_nrfd_n <= 1'b1;
            dev_ndac_n <= 1'b1;
            dev_dav_n  <= 1'b1;
            dev_eoi_n  <= 1'b1;
            dev_dio    <= 8'hFF;
            prev_bus_dav_n <= 1'b1;
            prev_atn_n     <= 1'b1;
        end else begin
            if (atn_active || listening) begin
                // Acceptor role. Abort any in-flight talker handshake. If
                // the CPU already READ the in-flight byte ($E820) but ATN
                // arrived before its NDAC release, the byte was delivered --
                // pop it now or the next TALK re-serves a duplicate.
                if (talk_st != T_IDLE) begin
                    if (talk_st == T_ACK && byte_consumed) begin
                        if (serving_status) txs_pop <= 1'b1;
                        else                tx_pop  <= 1'b1;
                    end
                    talk_st   <= T_IDLE;
                    dev_dav_n <= 1'b1;
                    dev_dio   <= 8'hFF;
                    dev_eoi_n <= 1'b1;
                end

                if (prev_atn_n && !cpu_atn_n) begin
                    dev_ndac_n <= 1'b0;    // join the handshake on ATN assert
                end

                if (prev_bus_dav_n && !bus_dav_n && !rx_full) begin
                    // Byte valid and room to store it: capture and accept.
                    // (While RX is full, hold NDAC asserted and wait -- IEEE
                    // flow control backpressures the CPU until the MCU drains
                    // a byte, instead of ACKing a command we silently drop.)
                    if (atn_active) begin
                        // Command byte: update protocol state and forward to MCU.
                        casez (~pia2_pb_out)
                            8'h3F:   listening <= 1'b0;                       // UNLISTEN
                            8'h5F:   talking <= 1'b0;                       // UNTALK (channel data persists)
                            8'b001?_????: listening <= (~pia2_pb_out & 8'h1E) == {3'b0, DEV_ADDR[4:1], 1'b0} ? 1'b1 : 1'b0;
                            8'b010?_????: talking   <= (~pia2_pb_out & 8'h1E) == {3'b0, DEV_ADDR[4:1], 1'b0} ? 1'b1 : 1'b0;
                            8'b011?_????: if (listening || talking) begin
                                sa <= ~pia2_pb_out;                           // secondary (data)
                                // Fresh status per request: the kernel re-reads
                                // channel 15 often; stale unread status bytes
                                // must not misalign the next read.
                                if (talking && (~pia2_pb_out & 8'h0F) == 4'hF)
                                    txs_flush <= 1'b1;
                            end
                            8'b1110_????: if (listening) begin
                                sa <= ~pia2_pb_out;                           // CLOSE
                                // Discard queued file data in fabric, before
                                // the MCU even sees the command -- but only
                                // for real data channels: ch15 CLOSE/OPEN is
                                // the DOS command channel and must not drop
                                // an in-flight stream (firmware ignores it).
                                if ((~pia2_pb_out & 8'h0F) != 4'hF)
                                    tx_flush <= 1'b1;
                            end
                            8'b1111_????: if (listening) begin
                                sa <= ~pia2_pb_out;                           // OPEN
                                if ((~pia2_pb_out & 8'h0F) != 4'hF)
                                    tx_flush <= 1'b1;
                            end
                            default: ;
                        endcase
                        rx_push <= 1'b1;
                        rx_push_data <= {1'b1, ~pia2_pb_out};
                    end else begin
                        rx_push <= 1'b1;
                        rx_push_data <= {1'b0, ~pia2_pb_out};
                    end
                    dev_nrfd_n <= 1'b0;
                    dev_ndac_n <= 1'b1;    // accepted
                end else if (!prev_bus_dav_n && bus_dav_n) begin
                    // DAV released: re-arm for the next byte.
                    dev_ndac_n <= 1'b0;
                    dev_nrfd_n <= 1'b1;
                end
            end else if (talking) begin
                // Talker role (source handshake). Bytes come from the TX FIFO;
                // if it runs dry mid-file the handshake simply pauses (the
                // CPU waits on DAV) until the MCU refills it.
                unique case (talk_st)
                    T_IDLE: begin
                        dev_ndac_n <= 1'b1;
                        dev_nrfd_n <= 1'b1;
                        if (serving_status ? !txs_empty : !tx_empty) talk_st <= T_DAV;
                    end
                    // Source handshake may only begin once an acceptor is
                    // present (NDAC asserted) and ready (NRFD released) --
                    // starting on NRFD alone loses the first byte if the
                    // acceptor hasn't configured NDAC yet.
                    T_DAV: if (bus_nrfd_n && !bus_ndac_n) begin
                        dev_dio   <= serving_status ? ~txs_mem[txs_rd][7:0] : ~tx_mem[tx_rd][7:0];
                        dev_eoi_n <= serving_status ? ~txs_mem[txs_rd][8]   : ~tx_mem[tx_rd][8];
                        dev_dav_n <= 1'b0;
                        byte_consumed <= 1'b0;
                        talk_st   <= T_ACK;
                    end
                    T_ACK: begin
                        if (cpu_data_strobe_i && !snap_we && snap_pia2
                            && pia_rs == 2'd0) begin
                            byte_consumed <= 1'b1;   // CPU read $E820
                        end
                        if (cpu_ndac_n) begin        // NDAC released: accept or terminate
                            if (byte_consumed) begin
                                if (serving_status) txs_pop <= 1'b1;
                                else                tx_pop  <= 1'b1;
                            end
                            dev_dav_n <= 1'b1;
                            dev_dio   <= 8'hFF;
                            dev_eoi_n <= 1'b1;
                            talk_st   <= T_REARM;
                        end
                    end
                    T_REARM: if (!cpu_ndac_n) begin     // acceptor re-armed
                        talk_st <= T_IDLE;
                    end
                endcase
            end else begin
                dev_nrfd_n <= 1'b1;
                dev_ndac_n <= 1'b1;
                dev_dav_n  <= 1'b1;
                dev_eoi_n  <= 1'b1;
                dev_dio    <= 8'hFF;
                talk_st    <= T_IDLE;
            end

            // Edge-retention: if the DAV falling edge landed while the RX
            // FIFO was full, the capture above was suppressed -- keep
            // prev_bus_dav_n high so the edge re-fires once the MCU drains
            // a slot, instead of being lost forever (bus deadlock).
            if (!(prev_bus_dav_n && !bus_dav_n && rx_full && (atn_active || listening)))
                prev_bus_dav_n <= bus_dav_n;
            prev_atn_n     <= cpu_atn_n;
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

    wire [7:0] status = {
        talk_starved,
        talking,
        listening,
        atn_active,
        tx_empty,
        tx_full,
        rx_mem[rx_rd][8],   // head-of-RX is an ATN command byte
        !rx_empty
    };

    logic rx_pop_req = 1'b0, tx_push_req = 1'b0, txs_push_req = 1'b0;
    logic mcu_tx_flush;
    logic [8:0] tx_push_data = '0;
    logic [8:0] txs_push_data = '0;

    always_ff @(posedge wb_clock_i) begin
        wbp_ack_o <= 1'b0;
        flush <= 1'b0;
        mcu_tx_flush <= 1'b0;
        rx_pop_req <= 1'b0;
        tx_push_req <= 1'b0;
        txs_push_req <= 1'b0;

        if (wb_req) begin
            wbp_ack_o <= 1'b1;
            unique case (reg_addr)
                IEEE_REG_CTRL: begin
                    // bit0 = enable; bit1 = tx_room (see TX_BURST_CHUNK above)
                    wbp_data_o <= {6'b0, tx_room, enable};
                    if (wbp_we_i) begin
                        enable       <= wbp_data_i[0];
                        flush        <= wbp_data_i[1];
                        mcu_tx_flush <= wbp_data_i[2];   // flush data TX only
                    end
                end
                IEEE_REG_STATUS: wbp_data_o <= status;
                // NOTE: the SPI bridge's pipelined read protocol prefetches
                // address+1 after every read, so register READS must be free
                // of side effects. Reading RX returns the head byte; popping
                // it requires an explicit WRITE to IEEE_REG_RX.
                IEEE_REG_RX: begin
                    wbp_data_o <= rx_mem[rx_rd][7:0];
                    if (wbp_we_i && !rx_empty) rx_pop_req <= 1'b1;
                end
                IEEE_REG_TX: begin
                    wbp_data_o <= 8'h00;   // write-only (data TX FIFO push)
                    if (wbp_we_i && !tx_full) begin
                        tx_push_req  <= 1'b1;
                        tx_push_data <= {1'b0, wbp_data_i};
                    end
                end
                IEEE_REG_TX_LAST: begin
                    wbp_data_o <= 8'h00;   // write-only (data TX FIFO push, EOI)
                    if (wbp_we_i && !tx_full) begin
                        tx_push_req  <= 1'b1;
                        tx_push_data <= {1'b1, wbp_data_i};
                    end
                end
                IEEE_REG_SA: wbp_data_o <= sa;
                IEEE_REG_TXS: begin
                    wbp_data_o <= 8'h00;   // write-only (status TX FIFO push)
                    if (wbp_we_i && !txs_full) begin
                        txs_push_req  <= 1'b1;
                        txs_push_data <= {1'b0, wbp_data_i};
                    end
                end
                IEEE_REG_TXS_LAST: begin
                    wbp_data_o <= 8'h00;   // write-only (status TX FIFO push, EOI)
                    if (wbp_we_i && !txs_full) begin
                        txs_push_req  <= 1'b1;
                        txs_push_data <= {1'b1, wbp_data_i};
                    end
                end
                default: wbp_data_o <= 8'h00;
            endcase
        end
    end

    // FIFO pointer/storage updates (single writer per FIFO side).
    always_ff @(posedge wb_clock_i) begin
        if (flush) begin
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

            if (txs_flush) begin
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
    wire dev_driving_dio = !dev_dav_n;   // we present data during our DAV

    wire [7:0] via_pb_composed = {
        bus_dav_n,          // PB7: DAV in
        bus_nrfd_n,         // PB6: NRFD in
        !vert_i,            // PB5: vertical retrace
        via_orb[4],         // PB4: cassette #2 motor (readback)
        via_orb[3],         // PB3: cassette write (readback)
        via_orb[2],         // PB2: ATN out (readback)
        via_orb[1],         // PB1: NRFD out (readback)
        bus_ndac_n          // PB0: NDAC in
    };

    wire [7:0] pia1_pa_composed = {
        diag_i,             // PA7: diagnostic sense (physical pin)
        dev_eoi_n,          // PA6: EOI in
        2'b11,              // PA5/PA4: cassette switch sense (none present)
        pia1_pa_out         // PA3-0: keyboard column select (readback)
    };

    always_ff @(posedge wb_clock_i) begin
        cpu_data_oe <= 1'b0;

        if (cpu_addr_strobe_i) cpu_data_oe <= 1'b0;
        if (enable && cpu_be_i && snap_valid && !snap_we) begin
            if (snap_pia2 && pia_rs == 2'd0) begin
                // $E820 DIO in: inject only while our talker drives data;
                // otherwise the (empty) physical bus reads $FF anyway.
                cpu_data_o  <= bus_dio;
                cpu_data_oe <= dev_driving_dio;
            end else if (snap_via && snap_rs == 4'd0) begin
                // $E840: full composed port B.
                cpu_data_o  <= via_pb_composed;
                cpu_data_oe <= 1'b1;
            end else if (snap_pia1 && pia_rs == 2'd0 && !dev_eoi_n) begin
                // $E810: only while asserting EOI (bit 6 low).
                cpu_data_o  <= pia1_pa_composed;
                cpu_data_oe <= 1'b1;
            end
        end
    end
endmodule
