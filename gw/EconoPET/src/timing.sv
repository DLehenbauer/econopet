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

`include "./src/common_pkg.svh"

import common_pkg::*;

module timing (
    input  logic sys_clock_i,
    output logic cpu_be_o,
    output logic cpu_clock_o,
    output logic clk1n_en_o,
    output logic clk2n_en_o,
    output logic clk8_en_o,
    output logic clk16_en_o,
    output logic wb_grant_o
);
    initial begin
        cpu_clock_o = 1'b0;
        cpu_be_o = 1'b0;
        wb_grant_o = 1'b0;
        clk1n_en_o = 1'b0;
        clk2n_en_o = 1'b0;
        clk8_en_o  = '0;
        clk16_en_o = '0;
    end

    localparam COUNTER_WIDTH = 6;

    logic [COUNTER_WIDTH-1:0] cycle_count = '0;

    always_ff @(posedge sys_clock_i) begin
        cycle_count <= cycle_count + 1'b1;
    end

    always_ff @(posedge sys_clock_i) begin
        clk8_en_o  <= cycle_count[2:0] == 3'b000;
        clk16_en_o <= cycle_count[1:0] == 2'b00;
    end

    function int ns_to_cycles(input int time_ns);
        // The '+1' is a conservative allowance for trace delay, etc.
        return int'($ceil(time_ns / common_pkg::mhz_to_ns(SYS_CLOCK_MHZ))) + 1;
    endfunction

    // Maximum number of 'sys_sys_clock_i' cycles required to complete an in-progress
    // wishbone transaction with RAM.
    localparam CPU_tBVD      = 30,  // CPU BE to Valid Data (tBVD)
               CPU_tPWH      = 62,  // CPU Clock Pulse Width High (tPWH)
               CPU_tDSR      = 15,  // CPU Data Setup Time (tDSR)
               CPU_tDHx      = 10,  // CPU Data Hold Time (tDHR, tDHW)
               CPU_tMDS      = 40,  // CPU Write Data Delay Time (tMDS)
               RAM_tAA       = 10,  // RAM Address Access Time (tAA)
               IOTX_t        = 11;  // IO Transciever Worst-Case Delay (tPZL)

    localparam bit [COUNTER_WIDTH-1:0] CPU_BE_START     = 0,
                                       CPU_PHI_START    = COUNTER_WIDTH'(int'(CPU_BE_START) + ns_to_cycles(CPU_tBVD + IOTX_t)),
                                       CLK1N            = CPU_PHI_END - 2,
                                       CPU_PHI_END      = COUNTER_WIDTH'(int'(CPU_PHI_START) + ns_to_cycles(CPU_tPWH)),
                                       CPU_BE_END       = COUNTER_WIDTH'(int'(CPU_PHI_END) + ns_to_cycles(CPU_tDHx)),
                                       WB_START         = COUNTER_WIDTH'(int'(CPU_BE_END) + ns_to_cycles(CPU_tBVD)),
                                       WB_END           = COUNTER_WIDTH'((1 << COUNTER_WIDTH) - 8),
                                       CLK2N            = CLK1N + COUNTER_WIDTH'(1 << (COUNTER_WIDTH - 1));

    always_ff @(posedge sys_clock_i) begin
        clk1n_en_o <= 0;
        clk2n_en_o <= 0;

        unique case (cycle_count)
            CPU_BE_START: begin
                cpu_be_o <= 1'b1;
            end
            CPU_PHI_START: begin
                cpu_clock_o <= 1'b1;
            end
            CLK1N: begin
                clk1n_en_o <= 1'b1;
                clk2n_en_o <= 1'b1;
            end
            CPU_PHI_END: begin
                cpu_clock_o <= 1'b0;
            end
            CPU_BE_END: begin
                cpu_be_o <= 1'b0;
            end
            WB_START: begin
                wb_grant_o <= 1'b1;
            end
            WB_END: begin
                wb_grant_o <= 1'b0;
            end
            CLK2N: begin
                clk2n_en_o <= 1'b1;
            end
            default: begin
            end
        endcase
    end
endmodule
