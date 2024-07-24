/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 *
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

`include "./sim/assert.svh"

module mock_bus #(
    parameter integer unsigned CPU_ADDR_WIDTH = 16,
    parameter integer unsigned DATA_WIDTH = 8
) (
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
    input logic cpu_we_n_i,

    // Incoming bus signals from 'mock_ram' module
    input logic [DATA_WIDTH-1:0] ram_data_i,
    input logic ram_oe_n_o,
    input logic ram_we_n_o,

    output logic [CPU_ADDR_WIDTH-1:0] bus_addr_o,
    output logic [DATA_WIDTH-1:0] bus_data_o,
    output logic bus_we_n_o
);
    // Arbitrate between FPGA and CPU driving address bus.
    wire top_driving_addr = top_addr_oe_i;                  // FPGA drives address bus when OE asserted.
    wire cpu_driving_addr = cpu_be_i;                       // CPU drives address bus when BE asserted.
    wire [1:0] addr_drivers = {cpu_driving_addr, top_driving_addr};

    always @(*) begin
        case (addr_drivers)
            2'b00: bus_addr_o = 'Z;
            2'b01: bus_addr_o = top_addr_i;
            2'b10: bus_addr_o = cpu_addr_i;
            default: bus_data_o = 'x;
        endcase
    end

    always_ff @(posedge clock_i) begin
        if (!$onehot0(addr_drivers)) begin
            $fatal(1, "[%0t] Multiple drivers on address bus. (fpga=%d, cpu=%d)", $time, top_driving_addr, cpu_driving_addr);
        end
    end

    // Arbitrate between FPGA, CPU, and RAM driving data bus.
    wire top_driving_data = top_data_oe_i;                  // FPGA drives data bus when OE asserted.
    wire cpu_driving_data = cpu_be_i && !cpu_we_n_i;        // CPU drives data bus when writing with BE asserted.
    wire ram_driving_data = !ram_oe_n_o && ram_we_n_o;      // RAM drives data bus when OE asserted without WE.
    wire [2:0] data_drivers = {ram_driving_data, cpu_driving_data, top_driving_data};

    always_comb begin
        case (data_drivers)
            3'b000: bus_data_o = 'z;
            3'b001: bus_data_o = top_data_i;
            3'b010: bus_data_o = cpu_data_i;
            3'b100: bus_data_o = ram_data_i;
            default: bus_data_o = 'x;
        endcase
    end

    always_ff @(posedge clock_i) begin
        if (!$onehot0(data_drivers)) begin
            $fatal(1, "[%0t] Multiple drivers on data bus. (fpga=%d, cpu=%d, ram=%d)", $time, top_driving_data, cpu_driving_data, ram_driving_data);
        end
    end

    // Arbitrate between FPGA and CPU driving WE signal.
    wire top_driving_we = top_we_n_oe_i;    // FPGA drives WE signal when OE asserted.
    wire cpu_driving_we = cpu_be_i;         // CPU drives WE signal when BE asserted.
    wire [1:0] we_drivers = {cpu_driving_we, top_driving_we};

    always_comb begin
        case (we_drivers)
            2'b00: bus_we_n_o = 'z;
            2'b01: bus_we_n_o = top_we_n_i;
            2'b10: bus_we_n_o = cpu_we_n_i;
            default: bus_we_n_o = 'x;
        endcase
    end

    always_ff @(posedge clock_i) begin
        if (!$onehot0(we_drivers)) begin
            $fatal(1, "[%0t] Multiple drivers on WE signal. (fpga=%d, cpu=%d)", $time, top_driving_we, cpu_driving_we);
        end
    end
endmodule
