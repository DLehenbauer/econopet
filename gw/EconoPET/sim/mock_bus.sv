// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

module mock_bus (
    input logic clock_i,

    // Incoming bus signals from FPGA 'top' module
    input logic [CPU_ADDR_WIDTH-1:0] top_addr_i,
    input logic top_addr_oe_i,
    input logic [DATA_WIDTH-1:0] top_data_i,
    input logic top_data_oe_i,
    input logic top_we_n_i,
    input logic top_we_n_oe_i,

    // Incoming bus signals from 'mock_cpu' module
    input logic cpu_be_i,
    input logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input logic [DATA_WIDTH-1:0] cpu_data_i,
    input logic cpu_data_oe_i,
    input logic cpu_we_n_i,

    // Incoming bus signals from SRAM
    input logic [DATA_WIDTH-1:0] ram_data_i,
    input logic ram_data_oe_i,

    // Incoming bus signals from IO
    input logic [DATA_WIDTH-1:0] io_data_i,
    input logic io_oe_n_i,

    output logic [CPU_ADDR_WIDTH-1:0] bus_addr_o,
    output logic [DATA_WIDTH-1:0] bus_data_o,
    output logic bus_data_oe_o,
    output logic bus_we_n_o
);
    // Arbitrate between FPGA and CPU driving address bus.
    wire top_driving_addr = top_addr_oe_i;                  // FPGA drives address bus when OE asserted.
    wire cpu_driving_addr = cpu_be_i;                       // CPU drives address bus when BE asserted.
    wire [1:0] addr_drivers = {cpu_driving_addr, top_driving_addr};

    always_comb begin
        case (addr_drivers)
            2'b01: bus_addr_o = top_addr_i;
            2'b10: bus_addr_o = cpu_addr_i;
`ifdef VERILATOR
            // For two-state simulators, use neutral values and track ownership separately.
            2'b00: bus_addr_o = '0;
            default: bus_addr_o = '0;
`else
            2'b00: bus_addr_o = 'z;
            default: bus_addr_o = 'x;
`endif
        endcase
    end

    always_ff @(posedge clock_i) begin
        if (!$onehot0(addr_drivers)) begin
            $fatal(1, "[%0t] Multiple drivers on address bus. (fpga=%d, cpu=%d)", $time, top_driving_addr, cpu_driving_addr);
        end
    end

    // Arbitrate between FPGA and CPU driving WE signal.
    wire top_driving_we = top_we_n_oe_i;    // FPGA drives WE signal when OE asserted.
    wire cpu_driving_we = cpu_be_i;         // CPU drives WE signal when BE asserted.
    wire [1:0] we_drivers = {cpu_driving_we, top_driving_we};

    always_comb begin
        case (we_drivers)
            // CT  C = CPU driving, T = FPGA driving
            2'b01: bus_we_n_o = top_we_n_i;
            2'b10: bus_we_n_o = cpu_we_n_i;
`ifdef VERILATOR
            // For two-state simulators, use neutral values and track ownership separately.
            2'b00: bus_we_n_o = 1'b1;
            default: bus_we_n_o = 1'b1;
`else
            2'b00: bus_we_n_o = 'z;
            default: bus_we_n_o = 'x;
`endif
        endcase
    end

    // Arbitrate between FPGA, CPU, SRAM, and I/O chips driving data bus.
    wire top_driving_data = top_data_oe_i;                  // FPGA drives data bus when OE asserted.
    wire cpu_driving_data = cpu_data_oe_i;                  // CPU model already qualifies this with BE.
    // Two-state simulators resolve the released ('z) bus_we_n_o to a defined
    // level mid-transition, which can flag false contention at the write-window
    // boundary. Judge I/O drive from whichever WE driver is enabled instead.
    wire eff_we_n = top_we_n_oe_i ? top_we_n_i
                  : cpu_be_i      ? cpu_we_n_i
                  :                 1'b1;    // released: no writer
    wire io_driving_data  = !io_oe_n_i && eff_we_n;         // I/O chips drive data bus when IO asserted and CPU/FPGA are reading.
    wire [3:0] data_drivers = {io_driving_data, ram_data_oe_i, cpu_driving_data, top_driving_data};

    always_comb begin
        bus_data_oe_o = 1'b0;
        bus_data_o    = {DATA_WIDTH{1'bx}};

        case (data_drivers)
            4'b0000: ;  // No driver. Leave bus_data_o as 'x' to catch unintentional reads.
            4'b0001: begin bus_data_o = top_data_i; bus_data_oe_o = 1'b1; end
            4'b0010: begin bus_data_o = cpu_data_i; bus_data_oe_o = 1'b1; end
            4'b0100: begin bus_data_o = ram_data_i; bus_data_oe_o = 1'b1; end
            4'b1000: begin bus_data_o = io_data_i;  bus_data_oe_o = 1'b1; end
            default: begin
                // Do not fail from combinational logic: logically simultaneous
                // enable changes can briefly appear multi-hot across delta cycles.
                // Drive 'x' here; the clocked check below rejects contention that
                // is still present at the bus protocol's sampling edge.
                bus_data_o = {DATA_WIDTH{1'bx}};
                bus_data_oe_o = 1'b1;
            end
        endcase
    end

    always_ff @(posedge clock_i) begin
        if (!$onehot0(data_drivers)) begin
            $fatal(1, "[%0t] Multiple drivers on data bus. (fpga=%d, cpu=%d, ram=%d, io=%d)", $time, top_driving_data, cpu_driving_data, ram_data_oe_i, io_driving_data);
        end
    end

    always_ff @(posedge clock_i) begin
        if (!$onehot0(we_drivers)) begin
            $fatal(1, "[%0t] Multiple drivers on WE signal. (fpga=%d, cpu=%d)", $time, top_driving_we, cpu_driving_we);
        end
    end
endmodule
