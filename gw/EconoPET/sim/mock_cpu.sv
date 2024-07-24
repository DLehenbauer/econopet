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

// Thin wrapper around Kenneth C. Dyke's 6502 core.
// https://github.com/kdyke/65xx
//
// This wrapper inverts WE, IRQ, NMI and RES to match the silicon.
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

    cpu6502 cpu(
        .clk(clock_i),
        .reset(!reset_n_i),
        .nmi(!nmi_n_i),
        .irq(!irq_n_i),
        .ready(ready_i),
        .write(cpu_we),
        .address(addr_o),
        .data_i(data_i),
        .data_o(data_o)
    );

    assign we_n_o = !cpu_we;
endmodule
