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

import common_pkg::*;

// Thin wrapper around Kenneth C. Dyke's 6502 core.
// https://github.com/kdyke/65xx
//
// This wrapper inverts WE, IRQ, NMI and RES to match the silicon.  It also delays
// the transition of ADDR, DOUT, and WE until one sys_clock cycle after the negative
// cpu_clock edge to mimic the timing of the 65C02.
module mock_cpu (
    input  logic sys_clock_i,               // FPGA system clock
    input  logic cpu_clock_i,               // CPU clock (PHI2)
    input  logic reset_n_i,
    output logic [CPU_ADDR_WIDTH-1:0] addr_o,
    input  logic     [DATA_WIDTH-1:0] data_i,
    output logic     [DATA_WIDTH-1:0] data_o,
    output logic we_n_o,
    input  logic irq_n_i,
    input  logic nmi_n_i,
    input  logic ready_i
);
    bit manual_mode = 1'b1;

    logic [CPU_ADDR_WIDTH-1:0] cpu_addr_next;
    logic [    DATA_WIDTH-1:0] cpu_data_next;
    logic                      cpu_we_next;

    cpu6502 cpu(
        .clk(cpu_clock_i),
        .reset(!reset_n_i),
        .nmi(!nmi_n_i),
        .irq(!irq_n_i),
        .ready(ready_i && !manual_mode),   // Suspend CPU when manual_mode is asserted
        .write(cpu_we_next),
        .address(cpu_addr_next),
        .data_i(data_i),
        .data_o(cpu_data_next)
    );

    logic [CPU_ADDR_WIDTH-1:0] set_addr_next;
    logic [    DATA_WIDTH-1:0] set_data_next;
    logic                      set_we_next;

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
        @(posedge cpu_clock_ne);

        set_addr_next = addr;
        set_data_next = data;
        set_we_next   = 1'b1;

        $display("[%t]   CPU Write %04x <- %02x", $time, addr, data);

        @(posedge cpu_clock_i);
    endtask

    task read(
        input  logic [CPU_ADDR_WIDTH-1:0] addr,
        output logic [    DATA_WIDTH-1:0] data
    );
        @(posedge cpu_clock_ne);

        set_addr_next = addr;
        set_data_next = 'x;
        set_we_next   = 1'b0;

        @(posedge cpu_clock_i);

        data = data_i;
        $display("[%t]   CPU Read %04x -> %02x", $time, addr, data);
    endtask

    logic cpu_clock_ne;

    // The 65C02 delays the transition of ADDR, DOUT, and WE until >10ns after the negative
    // clock edge.  We simulate this by capturing the next ADDR, DOUT, and WE one sys_clock
    // cycle after the negative cpu_clock dege.
    //
    // See: https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf, page 26

    edge_detect #(/* INITAL_DATA_I */ 0) clock_edge (
        .clock_i(sys_clock_i),
        .data_i(cpu_clock_i),
        .ne_o(cpu_clock_ne),
        .pe_o()
    );

    always_ff @(posedge sys_clock_i) begin
        if (cpu_clock_ne) begin
            addr_o <= manual_mode ? set_addr_next : cpu_addr_next;
            data_o <= manual_mode ? set_data_next : cpu_data_next;
            we_n_o <= !(manual_mode ? set_we_next : cpu_we_next);
        end
    end
endmodule
