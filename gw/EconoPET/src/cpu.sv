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
    output logic cpu_be_o,
    output logic cpu_clock_o
);
    initial begin
        cpu_be_o    = '0;
        cpu_clock_o = '0;
    end

    logic [5:0] cycle_count = '0;

    always_ff @(posedge sys_clock_i) begin
        if (cycle_count != '0 || cpu_grant_i) begin
            cycle_count <= cycle_count + 1'b1;
        end
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

    function bit [5:0] ns_to_cycles(input int time_ns);
        return 6'(int'($ceil(time_ns / common_pkg::mhz_to_ns(SYS_CLOCK_MHZ))));
    endfunction

    // Maximum number of 'sys_clock_i' cycles required to complete an in-progress
    // wishbone transaction with RAM.
    localparam MAX_WB_CYCLES = 5,
               CPU_tBVD      = 30,  // CPU BE to Valid Data (tBVD)
               CPU_tPWH      = 62,  // CPU Clock Pulse Width High (tPWH)
               CPU_tDSR      = 15,  // CPU Data Setup Time (tDSR)
               CPU_tDHx      = 10,  // CPU Data Hold Time (tDHR, tDHW)
               RAM_tAA       = 10,  // RAM Address Access Time (tAA)
               IOTX_t        = 11;  // IO Transciever Worst-Case Delay (tPZL)

    localparam bit [5:0] CPU_STALLED      = 0,
                         CPU_BE_START     = 1,
                         CPU_PHI_START    = 5,
                         CPU_PHI_END      = 9,
                         CPU_BE_END       = 13;

    always_ff @(posedge sys_clock_i) begin
        case (cycle_count)
            CPU_BE_START: begin         // In-progress transactions have drained.
                cpu_be_o <= 1'b1;
            end
            CPU_PHI_START: begin        // CPU setup time met.
                cpu_clock_o <= 1'b1;
            end
            CPU_PHI_END: begin          // CPU mimimum clock pulse width met.
                cpu_clock_o <= '0;
            end
            CPU_BE_END: begin           // CPU and I/O hold times met.  Start transition to high-Z.
                cpu_be_o <= '0;
            end
            default: begin
            end
        endcase
    end
endmodule
