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

module cpu (
    input  logic sys_clock_i,
    input  logic cpu_grant_i,
    input  logic cpu_we_i,
    
    output logic cpu_be_o,
    output logic cpu_clock_o,
    output logic cpu_addr_strobe_o,
    output logic cpu_data_strobe_o,
    output logic cpu_rd_en_o,
    output logic cpu_wr_en_o
);
    localparam COUNTER_WIDTH = 4;

    initial begin
        cpu_be_o            = '0;
        cpu_clock_o         = '0;
        cpu_addr_strobe_o   = '0;
        cpu_data_strobe_o   = '0;
    end

    // Timing for W65C02S
    // (See: https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf)
    //
    // Minumum Phi2 pulse width is high 62ms and low 63ms (tPWH, tPWL)
    //
    // Rising Phi2 edge triggers bus transfer:
    //  - BE must be asserted 45ns before rising Phi2 edge (tBVD + tDSR)
    //  - DIN must be valid 15ns before rising Phi2 edge (tDSR)
    //  - DIN must be held 10ns after rising Phi2 edge (tDHR)
    //  - BE must be held 10ns after rising Phi2 edge (tDHR, tDHW)
    //  - DOUT is valid 40ns after rising Phi2 edge (tMDS)
    //  
    // Falling Phi2 edge advances CPU to next state:
    //  - Previous ADDR, RWB, DOUT held 10ns after falling Phi2 edge (tAH, tDHW)
    //  - Next ADDR, RWB, DOUT valid 40ns after falling Phi2 edge (tADS, tMDS)
    //
    // Other:
    //  - At maximum clock rate, RAM has 70ns between ADDR stabilizing and the
    //    the beginning of the read setup time (tACC).
    //
    // Timing for W65C21
    // (See: https://www.westerndesigncenter.com/wdc/documentation/w65c21.pdf)
    //
    // - ADDR, RWB and CS2B must be valid 8ns before rising Phi2 edge (tACR, tACW)
    // - DOUT->DIN is valid 20ns after rising edge, well before CPU's falling edge (tCDR)
    // - DIN<-DOUT is required 10ns before falling edge and must be held 5ns after (tCDW, tHW)

    function int ns_to_cycles(input int time_ns);
        // The '+1' is a conservative allowance for trace delay, etc.
        return int'($ceil(time_ns / common_pkg::mhz_to_ns(SYS_CLOCK_MHZ))) + 1;
    endfunction

    // Maximum number of 'sys_clock_i' cycles required to complete an in-progress
    // wishbone transaction with RAM.
    localparam CPU_tBVD      = 30,  // CPU BE to Valid Data (tBVD)
               CPU_tPWH      = 62,  // CPU Clock Pulse Width High (tPWH)
               CPU_tDSR      = 15,  // CPU Data Setup Time (tDSR)
               CPU_tDHx      = 10,  // CPU Data Hold Time (tDHR, tDHW)
               CPU_tMDS      = 40,  // CPU Write Data Delay Time (tMDS)
               RAM_tAA       = 10,  // RAM Address Access Time (tAA)
               IOTX_t        = 11;  // IO Transciever Worst-Case Delay (tPZL)

    localparam bit [$bits(cycle_count)-1:0] CPU_SUSPENDED    = 0,
                                            CPU_BE_START     = 1,
                                            CPU_ADDR_VALID   = COUNTER_WIDTH'(int'(CPU_BE_START) + ns_to_cycles(CPU_tBVD)),
                                            CPU_PHI_START    = COUNTER_WIDTH'(int'(CPU_ADDR_VALID) + ns_to_cycles(IOTX_t)),
                                            CPU_DATA_VALID   = COUNTER_WIDTH'(int'(CPU_PHI_END) - ns_to_cycles(CPU_tDSR)),
                                            // To satisfy hold time, the falling edge of Phi2 must preceede falling edge of BE by tDHx.
                                            CPU_PHI_END      = COUNTER_WIDTH'(int'(CPU_BE_END) - ns_to_cycles(CPU_tDHx)),
                                            // To avoid bus contention, the falling edge of BE must preceede end of CPU cycle by tBVD.
                                            CPU_BE_END       = COUNTER_WIDTH'(16 - ns_to_cycles(CPU_tBVD));

    localparam bit [$bits(cycle_count)-1:0] CPU_PWH = CPU_PHI_END - CPU_PHI_START,
                                            CPU_DS = CPU_DATA_VALID - CPU_PHI_END;

    logic [COUNTER_WIDTH-1:0] cycle_count = CPU_SUSPENDED;

    // synthesis off
    initial begin
        if (int'(CPU_PWH) < ns_to_cycles(CPU_tPWH)) begin
            $fatal(1, "Error: CPU pulse width (%d) is less than minimum sys_clock cycles (%d)", CPU_PWH, ns_to_cycles(CPU_tPWH));
        end
    end
    // synthesis on

    always_ff @(posedge sys_clock_i) begin
        // CPU remains suspended until 'cpu_grant_i' is asserted.
        if (cycle_count != CPU_SUSPENDED || cpu_grant_i) begin
            cycle_count <= cycle_count + 1'b1;
        end
    end

    always_ff @(posedge sys_clock_i) begin
        cpu_addr_strobe_o <= 1'b0;
        cpu_data_strobe_o <= 1'b0;

        unique case (cycle_count)
            CPU_BE_START: begin             // In-progress transactions have drained.
                cpu_be_o <= 1'b1;
            end
            CPU_ADDR_VALID: begin           // CPU setup time met.
                cpu_addr_strobe_o <= 1'b1;
                cpu_rd_en_o <= !cpu_we_i;
            end
            CPU_PHI_START: begin            // CPU setup time met.
                cpu_clock_o <= 1'b1;
            end
            CPU_DATA_VALID: begin
                cpu_data_strobe_o <= 1'b1;
                cpu_wr_en_o <= cpu_we_i;
            end
            CPU_PHI_END: begin              // CPU minimum clock pulse width met.
                cpu_clock_o <= '0;
                cpu_wr_en_o <= '0;
            end
            CPU_BE_END: begin               // CPU and I/O hold times met.  Start transition to high-Z.
                cpu_rd_en_o <= '0;
                cpu_be_o <= '0;
            end
            default: begin
            end
        endcase
    end
endmodule
