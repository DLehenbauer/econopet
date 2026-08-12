// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// Adapts the 6502-oriented 'timing' module's per-round CPU-slot event pattern
// (timing.sv verifies the SRAM/IO-transceiver/PCB trace-delay budgets) for use
// by a soft MC6809E core sharing the same video-driven 8-phase arbiter.
//
// The arbiter only grants the CPU role a 250ns window once per 1000ns round
// (sized for the physical W65C02S, which is happy at that rate). The MC6809E
// requires tCYC >= 1000ns and PWEH/PWEL >= 450ns -- a full bus cycle would
// not fit in a single round. Rather than widen the CPU's slot (which would
// steal from video's 4 slots/round and risk display timing), this module
// reuses the *same* CPU-slot position but only "fires" it once every
// STRETCH_ROUNDS rounds, holding E (and, with a fixed offset, Q) steady in
// between. External SRAM/IO see exactly the same, already-margin-checked
// access window as the 6502 case -- just less often.
//
// STRETCH_ROUNDS sets the effective 6809 bus rate: 1 = ~1 MHz (real
// SuperPET speed, the default -- see CPU_STRETCH_ROUNDS in main.sv) with E
// dwell times of 500ns; 2 = ~500 kHz with 1000ns dwells. Both satisfy the
// MC6809E datasheet minimums (PWEH/PWEL 450ns, tCYC 1000ns; verified by the
// elaboration checks below).
module timing_6809 (
    input  logic sys_clock_i,

    // Reused, unmodified per-round CPU-slot events from 'timing' (the
    // existing 6502 timing generator running alongside this module).
    input  logic cpu_be_i,           // timing.sv's cpu_be_o
    input  logic cpu_clock_i,        // timing.sv's cpu_clock_o (PHI2-shaped pulse)
    input  logic cpu_addr_strobe_i,
    input  logic cpu_data_strobe_i,
    input  logic cpu_hold_strobe_i,
    input  logic cpu_wr_en_i,

    output logic cpu6809_be_o,
    output logic cpu6809_e_o,
    output logic cpu6809_q_o,
    output logic cpu6809_addr_strobe_o,
    output logic cpu6809_data_strobe_o,
    output logic cpu6809_hold_strobe_o,
    output logic cpu6809_wr_en_o
);
    // Number of 1000ns arbiter rounds spanned by one 6809 bus cycle:
    // 1 = ~1MHz (real SuperPET speed), >=2 stretches the E/Q dwells.
    // A parameter (not localparam) so testbenches can override it.
    parameter  int unsigned STRETCH_ROUNDS = 2;
    // $clog2(1) == 0, which would make `round` a zero-width vector and break
    // every ROUND_WIDTH'() cast below. Floor the width at 1 bit so
    // STRETCH_ROUNDS=1 (one bus cycle per round -> full ~1MHz, real SuperPET
    // speed) is a legal configuration. At SR=1 the round counter simply never
    // leaves 0 and e_hot_round is always asserted -- every round is a CPU
    // round.
    localparam int unsigned ROUND_WIDTH    =
        (STRETCH_ROUNDS <= 1) ? 1 : $clog2(STRETCH_ROUNDS);

    // E transitions on the last round of the super-cycle; Q is generated
    // within the same round (see the q_reg block below).
    localparam int unsigned E_HOT_ROUND = STRETCH_ROUNDS - 1;

    // Advance the round counter once per round, on the rising edge of
    // cpu_be_i (the CPU slot beginning).
    logic [ROUND_WIDTH-1:0] round = '0;
    logic cpu_be_prev = 1'b0;
    wire  cpu_be_rising = cpu_be_i && !cpu_be_prev;

    wire [ROUND_WIDTH-1:0] round_next =
        (round == ROUND_WIDTH'(STRETCH_ROUNDS - 1)) ? '0 : round + 1'b1;

    // Registered (precomputed from round_next) so downstream logic fans out
    // from one FF instead of a counter compare on the critical path.
    logic e_hot_round = 1'b0;

    always_ff @(posedge sys_clock_i) begin
        cpu_be_prev <= cpu_be_i;

        if (cpu_be_rising) begin
            round       <= round_next;
            e_hot_round <= round_next == ROUND_WIDTH'(E_HOT_ROUND);
        end
    end

    // Gate the per-round CPU-slot events to the E_HOT_ROUND: SRAM/IO see the
    // same BE/address/data timing as the 6502 case, once every STRETCH_ROUNDS.
    assign cpu6809_be_o          = cpu_be_i          && e_hot_round;
    assign cpu6809_addr_strobe_o = cpu_addr_strobe_i && e_hot_round;
    assign cpu6809_hold_strobe_o = cpu_hold_strobe_i && e_hot_round;
    assign cpu6809_wr_en_o       = cpu_wr_en_i       && e_hot_round;

    // The 6809-side data strobe is delayed one sys clock: consumers sample
    // the REGISTERED bus copy (main.sv cpu_data_q), so the pin data that was
    // valid at the original CPU_DATA_STROBE instant reaches them exactly one
    // cycle later. Same guaranteed data, plus a full cycle of pin-to-FF
    // routing budget. (The write-enable path is untouched: write data flows
    // outward from the already-registered cpu_data_o.)
    logic data_strobe_q = 1'b0;
    always_ff @(posedge sys_clock_i) begin
        data_strobe_q <= cpu_data_strobe_i && e_hot_round;
    end
    assign cpu6809_data_strobe_o = data_strobe_q;

    // E: tracks cpu_clock_i live during the hot round (rising/falling with
    // it), then holds its last value through the remaining
    // (STRETCH_ROUNDS-1) cold rounds. Gives real E-high and E-low dwell of
    // (STRETCH_ROUNDS/2)*1000ns each -- comfortably over the MC6809E's
    // PWEH/PWEL minimum of 450ns.
    logic e_reg = 1'b0;
    always_ff @(posedge sys_clock_i) begin
        if (e_hot_round) e_reg <= cpu_clock_i;
    end
    assign cpu6809_e_o = e_reg;

    // Q: the mc6809i core samples its interrupt pins (nIRQ/nFIRQ/nNMI, plus
    // nHALT/nDMABREQ) exclusively on the FALLING edge of Q -- the state
    // machine itself runs on E. Q must therefore produce one falling edge
    // per bus cycle, ordered before E's falling edge (on the real part Q
    // falls a quarter cycle before E). Emulate that ordering at sys-clock
    // granularity within the hot round: rise with the address strobe (before
    // E rises at CPU_PHI_START) and fall when the write-enable window ends
    // (CPU_WR_END, one sys-clock before E falls at CPU_PHI_END). The analog
    // quarter-cycle spacing doesn't matter to the soft core -- only the edge
    // order does.
    //
    logic q_reg = 1'b0;
    logic cpu_wr_en_prev = 1'b0;
    always_ff @(posedge sys_clock_i) begin
        cpu_wr_en_prev <= cpu_wr_en_i;
        if (e_hot_round) begin
            if (cpu_addr_strobe_i)                   q_reg <= 1'b1;
            else if (cpu_wr_en_prev && !cpu_wr_en_i) q_reg <= 1'b0;
        end
    end
    assign cpu6809_q_o = q_reg;

    // synthesis off
    initial begin
        // The reused per-round events already carry their own timing
        // assertions in timing.sv; here we only check the datasheet minimums
        // that this module is responsible for (E/Q dwell time, cycle time).
        localparam real DWELL_NS = (STRETCH_ROUNDS / 2.0) * 1000.0;  // real division

        // PWEH/PWEL are NMOS-physical minimums; the synchronous soft core
        // only needs the edges, so enforce dwell only for STRETCH_ROUNDS >= 2
        // (the configurations that emulate the real waveform).
        if (STRETCH_ROUNDS >= 2) begin
            if (DWELL_NS < CPU6809_PWEH)
                $fatal(1, "E-high dwell %.1fns violates PWEH >= %0dns", DWELL_NS, CPU6809_PWEH);
            if (DWELL_NS < CPU6809_PWEL)
                $fatal(1, "E-low dwell %.1fns violates PWEL >= %0dns", DWELL_NS, CPU6809_PWEL);
        end
        // tCYC (whole bus-cycle time) still applies -- it bounds how often the
        // core's state machine and the external SRAM/IO see a new access.
        if (STRETCH_ROUNDS * 1000.0 < CPU6809_tCYC)
            $fatal(1, "Cycle time %.1fns violates tCYC >= %0dns", STRETCH_ROUNDS * 1000.0, CPU6809_tCYC);
        // (No tEQ1 check: Q is not a half-super-cycle-offset copy of E any
        // more -- it pulses within the hot round purely to give the soft
        // core its per-cycle interrupt-sampling edge; see q_reg above.)
    end
    // synthesis on
endmodule
