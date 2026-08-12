// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// Soft 6551 ACIA at $EFF0-$EFF3 -- the SuperPET RS-232 port.
//
// Register map (RS1:RS0 = cpu_addr[1:0], confirmed against the CommonPET
// replica netlist: RS0=BA0, RS1=BA1, ~CS1=BA2 -- exactly four addresses,
// $EFF4-$EFF7 do NOT mirror):
//   0 read=RX data          write=TX data
//   1 read=status           write=programmed reset
//   2 command register      3 control register
//
// Behavioral notes (NMOS 6551 semantics -- SuperPETs shipped with NMOS
// parts; the modern W65C51N "TX-empty stuck" erratum is deliberately NOT
// replicated):
//   - Baud: internal generator only (the board's RxC pin goes to a test
//     point). baud = 1843200 / (16 * divisor), divisor from control[3:0].
//     Control bit 4 high selects the internal generator for RX as well --
//     SuperPET software must set it; RX runs at the TX rate in either case
//     since there is no external receive clock to fall back on.
//   - Status: b0 parity err, b1 framing err, b2 overrun, b3 RX full,
//     b4 TX empty, b5 ~DSR level, b6 ~DCD level, b7 IRQ (live level).
//     Reading status clears only the modem-change latch; RX clears on a
//     data read, TX on a data write. Writing status = programmed reset:
//     command bits 4:0 <= 5'b00010 (parity bits keep, echo off, RTS high,
//     RX IRQ disabled, DTR off), overrun cleared; control unchanged.
//   - Command: b0 DTR (1 = ~DTR asserted + receiver enabled), b1 RX IRQ
//     disable (0 = IRQ on RX full when DTR set), b3:2 TX control
//     (00 ~RTS high, no TX IRQ; 01 ~RTS low + TX IRQ; 10 ~RTS low, no TX
//     IRQ; 11 ~RTS low + transmit BREAK), b4 echo, b5 parity enable,
//     b7:6 parity mode (00 odd, 01 even, 10 mark, 11 space).
//   - Control: b3:0 baud, b4 RX clock source, b6:5 word length
//     (00=8, 01=7, 10=6, 11=5), b7 stop bits (0=1, 1=2).
//   - ~CTS high inhibits the transmitter (finishes the current character,
//     then holds). ~DCD/~DSR transitions set the IRQ flag regardless of the
//     IRQ enables (datasheet behavior); their levels read back in status.
//     Strap the inputs low (asserted) when unused -- HOSTCM and the
//     Waterloo terminal use in-band XON/XOFF and need only TXD/RXD.
//
// The Waterloo SETUP screen offers exactly the 14 internal rates (default
// 2400) with even/odd/mark/space parity and 1-2 stop bits; HOSTCM commonly
// runs 9600/even/1. All are covered by the general engines below.
module acia6551 (
    input  logic sys_clock_i,          // 64 MHz
    input  logic reset_i,

    // Soft-6809 bus (addresses from the soft core are stable for the whole
    // bus cycle, so decode is combinational on cpu_addr_i; actions fire on
    // the delayed data strobe, matching the registered cpu_data_i copy).
    input  logic                      cpu_be_i,
    input  logic                      cpu_data_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,
    output logic [    DATA_WIDTH-1:0] cpu_data_o,
    output logic                      cpu_data_oe,
    input  logic                      cpu_we_i,
    input  logic                      enable_i,       // hidden in MMU flat mode

    // Serial pins (PMOD)
    output logic txd_o,
    input  logic rxd_i,
    output logic rts_n_o,
    input  logic cts_n_i,
    output logic dtr_n_o,
    input  logic dsr_n_i,
    input  logic dcd_n_i,

    output logic irq_o                 // active high, wire-OR upstream
);
    wire sel = enable_i && cpu_be_i && (cpu_addr_i & 16'hFFFC) == 16'hEFF0;
    wire [1:0] rs = cpu_addr_i[1:0];

    // ------------------------------------------------------------------
    // Registers
    // ------------------------------------------------------------------
    logic [7:0] ctrl = 8'h00;
    logic [7:0] cmd  = 8'h00;

    logic [7:0] rx_data = 8'h00;
    logic rx_full = 1'b0;
    logic err_parity = 1'b0, err_framing = 1'b0, err_overrun = 1'b0;
    // The 6551 IRQ *pin* is a pure level of the enabled conditions; only the
    // modem-line (DSR/DCD) interrupt is an edge event that latches. Status
    // bit 7 mirrors the live pin -- it is NOT a read-cleared latch.
    logic modem_latch = 1'b0;

    logic [7:0] tx_hold = 8'h00;
    logic tx_empty = 1'b1;

    // Input synchronizers
    logic [1:0] rxd_sync = 2'b11, cts_sync = 2'b00, dsr_sync = 2'b00, dcd_sync = 2'b00;
    always_ff @(posedge sys_clock_i) begin
        rxd_sync <= {rxd_sync[0], rxd_i};
        cts_sync <= {cts_sync[0], cts_n_i};
        dsr_sync <= {dsr_sync[0], dsr_n_i};
        dcd_sync <= {dcd_sync[0], dcd_n_i};
    end
    wire rxd = rxd_sync[1];

    wire rx_enabled = cmd[0];                       // DTR set = receiver on
    wire rx_irq_en  = cmd[0] && !cmd[1];
    wire tx_irq_en  = cmd[3:2] == 2'b01;
    wire tx_break   = cmd[3:2] == 2'b11;

    assign dtr_n_o = !cmd[0];
    assign rts_n_o = cmd[3:2] == 2'b00;             // any non-00 asserts ~RTS

    // Word length in data bits, from control[6:5]
    wire [3:0] word_len = 4'd8 - {2'b00, ctrl[6:5]};
    wire parity_en = cmd[5];
    wire two_stop  = ctrl[7];

    // ------------------------------------------------------------------
    // Baud generation: one 16x-rate tick via a 32-bit NCO.
    // increment = 2^32 * (16 * baud) / 64e6, i.e. 2^32 * 1843200 / (div16 * 64e6)
    //
    // SBR 0 selects the external 16x clock, which goes to a test point on
    // the SuperPET -- the port is dead there. The TPUG driver can program
    // SBR 0 by accident (baud override reads a leftover module address), so
    // alias it to the terminal default of 1200.
    // ------------------------------------------------------------------
    function automatic logic [31:0] nco_inc(input logic [3:0] sbr);
        case (sbr)
            4'd0:  nco_inc = 32'd1288490;    // alias -> 1200 (see above)
            4'd1:  nco_inc = 32'd53687;      // 50
            4'd2:  nco_inc = 32'd80530;      // 75
            4'd3:  nco_inc = 32'd118016;     // 109.92
            4'd4:  nco_inc = 32'd144507;     // 134.58
            4'd5:  nco_inc = 32'd161061;     // 150
            4'd6:  nco_inc = 32'd322123;     // 300
            4'd7:  nco_inc = 32'd644245;     // 600
            4'd8:  nco_inc = 32'd1288490;    // 1200
            4'd9:  nco_inc = 32'd1932735;    // 1800
            4'd10: nco_inc = 32'd2576980;    // 2400
            4'd11: nco_inc = 32'd3865471;    // 3600
            4'd12: nco_inc = 32'd5153961;    // 4800
            4'd13: nco_inc = 32'd7730941;    // 7200
            4'd14: nco_inc = 32'd10307921;   // 9600
            4'd15: nco_inc = 32'd20615843;   // 19200
        endcase
    endfunction

    logic [31:0] nco = '0;
    logic tick16;                                   // one sys-clock pulse at 16x baud
    always_ff @(posedge sys_clock_i) begin
        { tick16, nco } <= {1'b0, nco} + {1'b0, nco_inc(ctrl[3:0])};
    end

    // ------------------------------------------------------------------
    // Transmitter
    // ------------------------------------------------------------------
    typedef enum logic [1:0] { TX_IDLE, TX_SHIFT } tx_state_t;
    tx_state_t tx_st = TX_IDLE;
    logic [11:0] tx_shift = '1;                     // start + data + parity + stop
    logic [3:0]  tx_bits = '0;
    logic [3:0]  tx_sub = '0;                       // 16x subdivision

    function automatic logic parity_bit(input logic [7:0] d, input logic [3:0] n);
        logic p;
        p = 1'b0;
        for (int i = 0; i < 8; i++) if (i < n) p ^= d[i];
        case (cmd[7:6])
            2'b00: parity_bit = ~p;                 // odd
            2'b01: parity_bit = p;                  // even
            2'b10: parity_bit = 1'b1;               // mark
            2'b11: parity_bit = 1'b0;               // space
        endcase
    endfunction

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) begin
            tx_st <= TX_IDLE;
            txd_o <= 1'b1;
            tx_sub <= '0;
        end else if (tx_break) begin
            txd_o <= 1'b0;
            tx_st <= TX_IDLE;
        end else begin
            case (tx_st)
                TX_IDLE: begin
                    txd_o <= 1'b1;
                    // ~CTS high inhibits starting a new character.
                    if (!tx_empty && !cts_sync[1]) begin
                        // Assemble LSB-first frame: start(0), data, [parity], stop(s)
                        logic [11:0] frame;
                        int pos;
                        frame = '1;
                        frame[0] = 1'b0;
                        for (int i = 0; i < 8; i++)
                            if (i < word_len) frame[1 + i] = tx_hold[i];
                        pos = 1 + word_len;
                        if (parity_en) begin
                            frame[pos] = parity_bit(tx_hold, word_len);
                            pos = pos + 1;
                        end
                        // stop bits are '1' which frame already holds
                        tx_shift <= frame;
                        tx_bits  <= 4'(1 + word_len + (parity_en ? 1 : 0)
                                       + (two_stop ? 2 : 1));
                        tx_sub   <= '0;
                        tx_empty <= 1'b1;           // holding reg free immediately
                        tx_st    <= TX_SHIFT;
                    end
                end
                TX_SHIFT: if (tick16) begin
                    if (tx_sub == 4'd15) begin
                        tx_sub <= '0;
                        txd_o  <= tx_shift[0];
                        tx_shift <= {1'b1, tx_shift[11:1]};
                        if (tx_bits == 0) tx_st <= TX_IDLE;
                        else tx_bits <= tx_bits - 1'b1;
                    end else begin
                        tx_sub <= tx_sub + 1'b1;
                    end
                end
                default: tx_st <= TX_IDLE;
            endcase

            if (tx_wr) begin
                tx_hold  <= cpu_data_i;
                tx_empty <= 1'b0;
            end
        end
    end

    // ------------------------------------------------------------------
    // Receiver: 16x oversampled, start-edge qualified, mid-bit sampling.
    // ------------------------------------------------------------------
    typedef enum logic [1:0] { RX_IDLE, RX_START, RX_SHIFT, RX_STOP } rx_state_t;
    rx_state_t rx_st = RX_IDLE;
    logic [7:0] rx_shift = '0;
    logic [3:0] rx_bits = '0;
    logic [3:0] rx_sub = '0;
    logic rx_parity_acc = 1'b0;
    logic rx_parity_seen = 1'b0;
    logic rx_taking_parity = 1'b0;

    logic dsr_prev = 1'b0, dcd_prev = 1'b0;

    wire tx_wr = sel && cpu_data_strobe_i && cpu_we_i && rs == 2'd0;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i || !rx_enabled) begin
            rx_st <= RX_IDLE;
        end else if (tick16) begin
            case (rx_st)
                RX_IDLE: if (!rxd) begin
                    rx_sub <= 4'd0;
                    rx_st  <= RX_START;
                end
                RX_START: begin
                    rx_sub <= rx_sub + 1'b1;
                    if (rx_sub == 4'd7) begin
                        if (!rxd) begin             // still low mid-start: valid
                            rx_sub  <= '0;
                            rx_bits <= '0;
                            rx_shift <= '0;
                            rx_parity_acc <= 1'b0;
                            rx_taking_parity <= 1'b0;
                            rx_st   <= RX_SHIFT;
                        end else begin
                            rx_st <= RX_IDLE;       // glitch
                        end
                    end
                end
                RX_SHIFT: begin
                    rx_sub <= rx_sub + 1'b1;
                    if (rx_sub == 4'd15) begin
                        if (!rx_taking_parity) begin
                            rx_shift <= {rxd, rx_shift[7:1]};
                            rx_parity_acc <= rx_parity_acc ^ rxd;
                            if (rx_bits == word_len - 1) begin
                                rx_taking_parity <= parity_en;
                                if (!parity_en) rx_st <= RX_STOP;
                            end
                            rx_bits <= rx_bits + 1'b1;
                        end else begin
                            rx_parity_seen <= rxd;
                            rx_st <= RX_STOP;
                        end
                    end
                end
                RX_STOP: begin
                    rx_sub <= rx_sub + 1'b1;
                    if (rx_sub == 4'd15) begin
                        // One stop bit is enough to close the frame (the
                        // second, if configured, just extends idle time).
                        logic [7:0] aligned;
                        logic p_err;
                        aligned = rx_shift >> (4'd8 - word_len);
                        case (cmd[7:6])
                            2'b00:   p_err = (rx_parity_acc ^ rx_parity_seen) == 1'b0; // odd
                            2'b01:   p_err = (rx_parity_acc ^ rx_parity_seen) == 1'b1; // even
                            default: p_err = 1'b0;                    // mark/space: not checked
                        endcase
                        if (rx_full) err_overrun <= 1'b1;
                        else begin
                            rx_data     <= aligned;
                            err_parity  <= parity_en && p_err;
                            err_framing <= !rxd;                       // stop bit must be 1
                            rx_full     <= 1'b1;
                        end
                        // rx_full is the RX interrupt condition (level); the
                        // pin asserts while rx_full && rx_irq_en and drops
                        // when the CPU reads the data register.
                        rx_st <= RX_IDLE;
                    end
                end
            endcase
        end

        // ~DSR / ~DCD transitions latch a modem interrupt (edge event).
        if (dsr_sync[1] != dsr_prev || dcd_sync[1] != dcd_prev) modem_latch <= 1'b1;
        dsr_prev <= dsr_sync[1];
        dcd_prev <= dcd_sync[1];



        // --------------------------------------------------------------
        // CPU register access
        // --------------------------------------------------------------
        if (sel && cpu_data_strobe_i) begin
            if (cpu_we_i) begin
                case (rs)
                    2'd1: begin                     // programmed reset
                        cmd <= {cmd[7:5], 5'b00010};
                        err_overrun <= 1'b0;
                    end
                    2'd2: cmd  <= cpu_data_i;
                    2'd3: ctrl <= cpu_data_i;
                    default: ;                      // rs 0 handled by tx_wr
                endcase
            end else begin
                case (rs)
                    2'd0: begin                     // RX data read
                        rx_full <= 1'b0;
                        err_parity <= 1'b0;
                        err_framing <= 1'b0;
                        err_overrun <= 1'b0;
                    end
                    // Status read acknowledges the modem edge-latch; the
                    // RX/TX interrupt conditions are levels and clear only
                    // when the CPU services data (read/write) or disables
                    // the interrupt -- exactly as a real 6551.
                    2'd1: modem_latch <= 1'b0;
                    default: ;
                endcase
            end
        end

        if (reset_i) begin
            ctrl <= 8'h00;
            cmd  <= 8'h00;
            rx_full <= 1'b0;
            err_parity <= 1'b0; err_framing <= 1'b0; err_overrun <= 1'b0;
            modem_latch <= 1'b0;
        end
    end

    // ------------------------------------------------------------------
    // Read injection (registered, same style as ieee.sv)
    //
    // The IRQ pin is a level: (RDRF & RX enabled) | (TDRE & TX enabled) |
    // the modem-change latch. Enabling the TX interrupt with the transmit
    // register already empty interrupts immediately (the TPUG bridge's
    // queue/enable/sleep idiom depends on it); the service ends the level
    // by writing data or disabling the enable. Only the modem latch is
    // cleared by a status read.
    // ------------------------------------------------------------------
    // Bit order per the WATCOM SuperPET Serial Port manual (status $EFF1):
    // 7=IRQ 6=~DSR 5=~DCD 4=TDRE 3=RDRF 2=OVRN 1=FE 0=PE. The TPUG driver's
    // transmit gate masks exactly ~DSR/~DCD, so both must read ready (0).
    // IRQ pin = pure level of the enabled conditions + modem edge-latch.
    wire irq_level = (rx_full && rx_irq_en)          // RDRF & RX-int-enabled
                   || (tx_empty && tx_irq_en)        // TDRE & TX-int-enabled
                   || modem_latch;                   // DSR/DCD edge event

    wire [7:0] status = { irq_level, dsr_sync[1], dcd_sync[1], tx_empty,
                          rx_full, err_overrun, err_framing, err_parity };

    always_ff @(posedge sys_clock_i) begin
        cpu_data_oe <= 1'b0;
        if (sel && !cpu_we_i) begin
            cpu_data_oe <= 1'b1;
            case (rs)
                2'd0: cpu_data_o <= rx_data;
                2'd1: cpu_data_o <= status;
                2'd2: cpu_data_o <= cmd;
                2'd3: cpu_data_o <= ctrl;
            endcase
        end
    end

    assign irq_o = irq_level;
endmodule
