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

// Thin wrapper around Verilog-6502 by Arlet Ottens:
// https://github.com/Arlet/verilog-6502
//
// This wrapper inverts WE, IRQ, NMI and RES to match the silicon.  It also
// registers output signals on the rising clock edge to avoid glitches.
module mock_cpu #(
    parameter integer unsigned ADDR_WIDTH = 16,
    parameter integer unsigned DATA_WIDTH = 8
) (
    input  logic clock_i,
    input  logic reset_n_i,
    output logic [ADDR_WIDTH-1:0] addr_o,
    input  logic [DATA_WIDTH-1:0] data_i,
    output logic [DATA_WIDTH-1:0] data_o,
    output logic we_n_o,
    input  logic irq_n_i,
    input  logic nmi_n_i,
    input  logic ready_i
);
    logic [ADDR_WIDTH-1:0] cpu_addr_o;
    logic [DATA_WIDTH-1:0] cpu_data_o;
    logic cpu_we;

    cpu cpu(
        .clk(clock_i),      // CPU clock 
        .reset(!reset_n_i), // reset signal (0 = normal, 1 = reset)
        .AB(cpu_addr_o),    // address bus
        .DI(data_i),        // data in, read bus
        .DO(cpu_data_o),    // data out, write bus
        .WE(cpu_we),        // write enable (0 = read, 1 = write)
        .IRQ(!irq_n_i),     // interrupt request (0 = normal, 1 = interrupt)
        .NMI(!nmi_n_i),     // non-maskable interrupt request  (0 = normal, 1 = interrupt)
        .RDY(ready_i)       // Ready signal (0 = pause, 1 = ready)
    );

    always_ff @(posedge clock_i) begin
        // A real 6502 reads DI on the rising edge and transitions ADDR, DO, and WE
        // to the next state on the falling edge.
        //
        // The Verilog-6502 module transitions ADDR, DO, and WE on the rising edge
        // and reads DI on the following rising edge.  To avoid glitches, we need
        // to register the output signals on the rising edge.
        addr_o <= cpu_addr_o;
        data_o <= cpu_data_o;
        we_n_o <= !cpu_we;
    end
endmodule
