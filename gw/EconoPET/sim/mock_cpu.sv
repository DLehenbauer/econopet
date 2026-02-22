// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// Thin wrapper around the m6502 CPU core.
// https://github.com/chrismoos/m6502
module mock_cpu (
    input  logic cpu_clock_i,   // CPU clock (PHI2)
    input  logic reset_n_i,
    output logic [CPU_ADDR_WIDTH-1:0] addr_o,
    input  logic     [DATA_WIDTH-1:0] data_i,
    output logic     [DATA_WIDTH-1:0] data_o,
    output logic we_n_o,
    output logic sync_o,
    input  logic irq_n_i,
    input  logic nmi_n_i,
    input  logic ready_i
);
    bit manual_mode = 1'b1;

    logic [CPU_ADDR_WIDTH-1:0] cpu_addr;
    logic [    DATA_WIDTH-1:0] cpu_data;
    logic                      cpu_rw;

    cpu_6502 cpu(
        .i_clk(cpu_clock_i),
        .o_phi1(),
        .o_phi2(),
        .i_reset_n(reset_n_i),
        .i_rdy(ready_i && !manual_mode),
        .i_nmi_n(nmi_n_i),
        .i_irq_n(irq_n_i),
        .i_so_n(1'b1),
        .o_sync(),
        .i_bus_data(data_i),
        .o_bus_data(cpu_data),
        .o_bus_addr(cpu_addr),
        .o_rw(cpu_rw),
        .i_debug_sel(3'b0),
        .o_debug_data()
    );

    logic [CPU_ADDR_WIDTH-1:0] set_addr;
    logic [    DATA_WIDTH-1:0] set_data;
    logic                      set_we;

    task start();
        manual_mode = 1'b0;
    endtask

    task stop();
        manual_mode = 1'b1;
    endtask

    task write(
        input  logic [CPU_ADDR_WIDTH-1:0] addr,
        input  logic [    DATA_WIDTH-1:0] data
    );
        @(negedge cpu_clock_i);

        set_addr = addr;
        set_data = data;
        set_we   = 1'b1;

        $display("[%t]   CPU Write %04x <- %02x", $time, addr, data);

        @(posedge cpu_clock_i);

        // Return to read mode after write completes to avoid unintended
        // repeated writes on subsequent bus cycles
        @(negedge cpu_clock_i);
        set_we = 1'b0;
    endtask

    task read(
        input  logic [CPU_ADDR_WIDTH-1:0] addr,
        output logic [    DATA_WIDTH-1:0] data
    );
        @(negedge cpu_clock_i);

        set_addr = addr;
        set_data = 'x;
        set_we   = 1'b0;

        // The W65C02S latches read data near the falling edge of PHI2 (data
        // must be valid tDSR before that edge). Capture at negedge to give
        // external devices (SRAM, PIA, etc.) the full PHI2-high period to
        // drive valid data onto the bus.
        @(posedge cpu_clock_i);
        @(negedge cpu_clock_i);

        data = data_i;
        $display("[%t]   CPU Read %04x -> %02x", $time, addr, data);
    endtask

    // The real NMOS 6502 produces ADDR, RW, and SYNC shortly after the positive PHI2
    // transition and latches the next instruction on the following negative edge.  In
    // between there is enough time for us to inspect DIN and deassert RDY if the next
    // instruction is a breakpoint.
    //
    // The m6502 model's 'o_sync' lags 'first_microinstruction' by a half cycle,
    // so it is not asserted until the end of the opcode-fetch cycle (too late for
    // breakpoint logic to react).  We use 'first_microinstruction' instead, which
    // is set on the preceding negedge and therefore already high when PHI2 rises
    // to start the fetch (matching real hardware timing).
    wire cpu_sync_early = cpu.first_microinstruction
                       && !cpu.handle_irq
                       && !cpu.handle_nmi;

    assign addr_o = manual_mode ? set_addr : cpu_addr;
    assign data_o = manual_mode ? set_data : cpu_data;
    assign we_n_o = manual_mode ? !set_we  : cpu_rw;
    assign sync_o = manual_mode ? 1'b0 : cpu_sync_early;
endmodule
