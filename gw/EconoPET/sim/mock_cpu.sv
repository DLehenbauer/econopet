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
// This wrapper inverts WE, IRQ, NMI and RES to match the silicon.  It also delays
// the transition of ADDR, DOUT, and WE until one sys_clock cycle after the negative
// cpu_clock edge to mimic the timing of the 65C02.
module mock_cpu #(
    parameter integer unsigned ADDR_WIDTH = 16,
    parameter integer unsigned DATA_WIDTH = 8
) (
    input  logic sys_clock_i,               // FPGA system clock
    input  logic cpu_clock_i,               // CPU clock (PHI2)
    input  logic reset_n_i,
    output logic [ADDR_WIDTH-1:0] addr_o,
    input  logic [DATA_WIDTH-1:0] data_i,
    output logic [DATA_WIDTH-1:0] data_o,
    output logic we_n_o,
    input  logic irq_n_i,
    input  logic nmi_n_i,
    input  logic ready_i
);
    logic [ADDR_WIDTH-1:0] addr_next;
    logic [DATA_WIDTH-1:0] data_next;
    logic we_next;

    cpu6502 cpu(
        .clk(cpu_clock_i),
        .reset(!reset_n_i),
        .nmi(!nmi_n_i),
        .irq(!irq_n_i),
        .ready(ready_i),
        .write(we_next),
        .address(addr_next),
        .data_i(data_i),
        .data_o(data_next)
    );

    logic cpu_clock_ne;

    // The 65C02 delays the transition of ADDR, DOUT, and WE until >10ns after the negative
    // clock edge.  We simulate this by capturing the next ADDR, DOUT, and WE one sys_clock
    // cycle after the negative cpu_clock dege.
    //
    // See: https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf, page 26

    edge_detect #(/* INITAL_DATA_I */ 0) clock_edge (
        .clock_i(sys_clock_i),
        .data_i(cpu_clock_i),
        .ne_o(cpu_clock_ne)
    );

    always_ff @(posedge sys_clock_i) begin
        if (cpu_clock_ne) begin
            addr_o <= addr_next;
            data_o <= data_next;
            we_n_o <= !we_next;
        end
    end
endmodule
