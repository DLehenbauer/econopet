// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// Adapts the 6502-oriented 'timing' module's per-round CPU-slot event pattern
// (timing.sv verifies the SRAM/IO-transceiver/PCB trace-delay budgets) for use
// by a soft MC6809E core sharing the same video-driven 8-phase arbiter.
//
// The soft 6809 runs one bus cycle per 1000ns arbiter round (~1 MHz, real
// SuperPET speed). The MC6809E's tCYC >= 1000ns is met exactly; its
// PWEH/PWEL dwell minimums are NMOS-physical requirements the synchronous
// soft core does not share -- it only needs the E/Q edge ordering.
module timing_6809 (
    input  logic sys_clock_i,
    input  logic cpu_ready_i,
    input  logic cpu_reset_i,

    // Reused, unmodified per-round CPU-slot events from 'timing' (the
    // existing 6502 timing generator running alongside this module).
    input  logic cpu_be_i,           // timing.sv's cpu_be_o
    input  logic cpu_clock_i,        // timing.sv's cpu_clock_o (PHI2-shaped pulse)
    input  logic cpu_addr_strobe_i,
    input  logic cpu_hold_strobe_i,
    input  logic cpu_wr_en_i,

    output logic cpu6809_be_o,
    output logic cpu6809_e_o,
    output logic cpu6809_q_o,
    output logic cpu6809_addr_strobe_o,
    output logic cpu6809_hold_strobe_o,
    output logic cpu6809_wr_en_o,
    output logic cpu6809_halted_o
);
    logic cycle_enabled = 1'b0;
    logic e_reg = 1'b0;
    logic q_reg = 1'b0;

    // Keep clocking while reset is asserted so the core can sample nRESET on
    // falling E.
    wire cycle_request = cpu_ready_i || cpu_reset_i;

    // Firmware changes READY/reset during Wishbone SPI slots, outside the CPU
    // slot, so the request can directly enable or pause whole 6809 cycles.
    // Reset still needs clock edges because the core samples nRESET on falling E.
    always_ff @(posedge sys_clock_i) begin
        cycle_enabled <= cycle_request;
    end
    assign cpu6809_halted_o = !cycle_enabled && !e_reg && !q_reg;

    // The per-round CPU-slot events pass straight through: SRAM/IO see the
    // same BE/address/data timing as the 6502 case.
    assign cpu6809_be_o          = cpu_be_i && cycle_enabled;
    assign cpu6809_addr_strobe_o = cpu_addr_strobe_i && cycle_enabled;
    assign cpu6809_hold_strobe_o = cpu_hold_strobe_i && cycle_enabled;
    assign cpu6809_wr_en_o       = cpu_wr_en_i && cycle_enabled;

    // E: registered copy of cpu_clock_i (the PHI2-shaped pulse), so the E
    // edges land one sys clock after the arbiter's. main.sv provides the
    // shared captured-data strobe used by both soft cores.
    always_ff @(posedge sys_clock_i) begin
        e_reg <= cycle_enabled && cpu_clock_i;
    end
    assign cpu6809_e_o = e_reg;

    // Q: mc6809i samples its interrupt pins (nIRQ/nFIRQ/nNMI, nHALT,
    // nDMABREQ) on the falling edge of Q, while the state machine runs on
    // E. Q must therefore fall once per bus cycle, before E falls: it rises
    // with the address strobe and falls one sys clock after the
    // write-enable window closes, still ahead of the registered E's fall.
    logic cpu_wr_en_prev = 1'b0;
    always_ff @(posedge sys_clock_i) begin
        cpu_wr_en_prev <= cpu_wr_en_i;
        if (!cycle_enabled)                      q_reg <= 1'b0;
        else if (cpu_addr_strobe_i)              q_reg <= 1'b1;
        else if (cpu_wr_en_prev && !cpu_wr_en_i) q_reg <= 1'b0;
    end
    assign cpu6809_q_o = q_reg;

    // synthesis off
    logic cycle_request_prev = 1'b0;
    always_ff @(posedge sys_clock_i) begin
        if (cycle_request != cycle_request_prev)
            assert (!cpu_be_i && !e_reg && !q_reg)
                else $fatal(1, "6809 run request changed during an active CPU cycle");
        cycle_request_prev <= cycle_request;
    end

    initial begin
        // The reused per-round events carry their own timing assertions in
        // timing.sv. The one datasheet minimum this module owns: the 1000ns
        // round meets the MC6809E's tCYC >= 1000ns exactly. (No PWEH/PWEL
        // dwell or tEQ1 checks: those are NMOS-physical requirements; the
        // synchronous soft core only needs the edge ordering above.)
        if (1000.0 < CPU6809_tCYC)
            $fatal(1, "1000ns round violates tCYC >= %0dns", CPU6809_tCYC);
    end
    // synthesis on
endmodule
