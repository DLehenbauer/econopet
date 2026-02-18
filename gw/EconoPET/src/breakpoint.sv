// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

//
// Detects when the CPU fetches the STP opcode ($DB) and halts the CPU by
// deasserting RDY before the falling PHI2 edge.  This allows the MCU to
// implement unlimited software breakpoints by patching SRAM with STP.
//
// See docs/dev/breakpoint.md for the full design.
//
module breakpoint (
    input  logic                      sys_clock_i,

    // CPU bus signals
    input  logic                      cpu_sync_i,            // High during opcode fetch (T1 cycle)
    input  logic [   DATA_WIDTH-1:0]  cpu_data_i,            // Instruction byte fetched by the CPU
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,            // CPU address bus

    // Timing signals from timing.sv
    input  logic                      cpu_data_strobe_i,     // One-cycle pulse when data bus is valid
    input  logic                      cpu_be_i,              // Bus enable (high when CPU owns the bus)

    // MCU-controlled signals
    input  logic                      cpu_ready_i,           // RDY from register_file (MCU control)
    input  logic                      clear_i,               // Pulse to clear breakpoint halt

    // Outputs
    output logic                      cpu_ready_o,           // Gated RDY to CPU (deasserted on halt)
    output logic                      halted_o,              // Status: 1 = CPU halted on a breakpoint
    output logic [CPU_ADDR_WIDTH-1:0] addr_o                 // Latched CPU address at breakpoint
);
    localparam logic [DATA_WIDTH-1:0] STP_OPCODE = 8'hDB;

    logic halted = 1'b0;
    logic [CPU_ADDR_WIDTH-1:0] bp_addr = '0;

    always_ff @(posedge sys_clock_i) begin
        if (clear_i) begin
            halted <= 1'b0;
        end else if (cpu_data_strobe_i && cpu_be_i && cpu_sync_i && cpu_data_i == STP_OPCODE) begin
            halted  <= 1'b1;
            bp_addr <= cpu_addr_i;
        end
    end

    assign cpu_ready_o = cpu_ready_i && !halted;
    assign halted_o    = halted;
    assign addr_o      = bp_addr;
endmodule
