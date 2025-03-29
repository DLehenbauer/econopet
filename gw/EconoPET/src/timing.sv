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
    output logic cpu_wr_strobe_o,
    output logic load_sr1_o,
    output logic load_sr2_o,
    output logic clk8_en_o,
    output logic clk16_en_o,
    output logic [0:0] grant_o,
    output logic grant_valid_o
);
    initial begin
        cpu_clock_o     = 1'b0;
        cpu_be_o        = 1'b0;
        cpu_wr_strobe_o = 1'b0;
        clk8_en_o       = 1'b0;
        clk16_en_o      = 1'b0;
    end

    localparam COUNTER_WIDTH = 6;

    logic [COUNTER_WIDTH-1:0] cycle_count = 6'b111111;

    always_ff @(posedge sys_clock_i) begin
        cycle_count <= cycle_count + 1'b1;
    end

    logic [2:0] grant = 3'b111;

    always_ff @(posedge sys_clock_i) begin
        clk8_en_o <= '0;

        if (cycle_count[2:0] == 3'b000) begin
            clk8_en_o  <= 1'b1;
            grant      <= grant + 1'b1;
        end
            
        clk16_en_o <= cycle_count[1:0] == 2'b00;
    end

    localparam VIDEO_1 = 3'd0,
               VIDEO_2 = 3'd1,
               SPI_1   = 3'd2,
               VIDEO_3 = 3'd3,
               VIDEO_4 = 3'd4,
               SPI_2   = 3'd5,
               CPU_1   = 3'd6,
               CPU_2   = 3'd7;

    assign grant_valid_o = grant != CPU_1 && grant != CPU_2 && clk8_en_o;
    assign grant_o = (grant == SPI_1 || grant == SPI_2) ? 1'b1 : 1'b0;
    
    assign load_sr1_o = grant == CPU_1 && clk8_en_o;
    assign load_sr2_o = (grant == CPU_1 || grant == SPI_1) && clk8_en_o;

    function bit [COUNTER_WIDTH-1:0] ns_to_cycles(input int time_ns);
        // The '+1' is a conservative allowance for trace delay, etc.
        return COUNTER_WIDTH'(common_pkg::ns_to_cycles(time_ns)) + 1;
    endfunction

    localparam CPU_tBVD      = 30,  // CPU BE to Valid Data (tBVD)
               CPU_tPWH      = 62,  // CPU Clock Pulse Width High (tPWH)
               CPU_tDSR      = 15,  // CPU Data Setup Time (tDSR)
               CPU_tDHx      = 10,  // CPU Data Hold Time (tDHR, tDHW)
               CPU_tMDS      = 40,  // CPU Write Data Delay Time (tMDS)
               RAM_tAA       = 10,  // RAM Address Access Time (tAA)
               IOTX_t        = 11;  // IO Transceiver Worst-Case Delay (tPZL)

    localparam bit [COUNTER_WIDTH-1:0] CPU_OFFSET = CPU_1 * 8;

    localparam bit [COUNTER_WIDTH-1:0] CPU_BE_START     = CPU_OFFSET,
                                       CPU_PHI_START    = CPU_BE_START + ns_to_cycles(CPU_tBVD + IOTX_t),
                                       CPU_WR_STROBE    = CPU_PHI_END - 2,
                                       CPU_PHI_END      = CPU_PHI_START + ns_to_cycles(CPU_tPWH),
                                       CPU_BE_END       = CPU_PHI_END + ns_to_cycles(CPU_tDHx);

    always_ff @(posedge sys_clock_i) begin
        cpu_wr_strobe_o <= '0;

        unique case (cycle_count)
            CPU_BE_START: begin
                cpu_be_o <= 1'b1;
            end
            CPU_PHI_START: begin
                cpu_clock_o <= 1'b1;
            end
            CPU_WR_STROBE: begin
                cpu_wr_strobe_o <= 1'b1;
            end
            CPU_PHI_END: begin
                cpu_clock_o <= 1'b0;
            end
            CPU_BE_END: begin
                cpu_be_o <= 1'b0;
            end
            default: /* do nothing */;
        endcase
    end
endmodule
