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
    logic we;

    cpu cpu(
        .clk(clock_i),          // CPU clock 
        .reset(!reset_n_i),     // reset signal (0 = normal, 1 = reset)
        .AB(addr_o),            // address bus
        .DI(data_i),            // data in, read bus
        .DO(data_o),            // data out, write bus
        .WE(we),                // write enable (0 = read, 1 = write)
        .IRQ(!irq_n_i),         // interrupt request (0 = normal, 1 = interrupt)
        .NMI(!nmi_n_i),         // non-maskable interrupt request  (0 = normal, 1 = interrupt)
        .RDY(ready_i)           // Ready signal (0 = pause, 1 = ready)
    );

    assign we_n_o = !we;
endmodule
