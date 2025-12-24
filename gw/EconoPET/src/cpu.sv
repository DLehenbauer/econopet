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
    // | Symbol | Parameter                       | Min | Max | Units |
    // |--------|---------------------------------|-----|-----|-------|
    // | tACC   | Access Time                     |  70 |   - | ns    |
    // | tAH    | Address Hold Time               |  10 |   - | ns    |
    // | tADS   | Address Setup Time              |   - |  40 | ns    |
    // | tBVD   | BE to Valid Data (1)            |   - |  30 | ns    |
    // | tPWH   | Clock Pulse Width High          |  62 |   - | ns    |
    // | tPWL   | Clock Pulse Width Low           |  63 |   - | ns    |
    // | tCYC   | Cycle Time (3)                  | 125 |   - | ns    |
    // | tF,tR  | Fall Time, Rise Time            |   - |   5 | ns    |
    // | tPCH   | Processor Control Hold Time     |  10 |   - | ns    |
    // | tPCS   | Processor Control Setup Time    |  15 |   - | ns    |
    // | tDHR   | Read Data Hold Time             |  10 |   - | ns    |
    // | tDSR   | Read Data Setup Time            |  15 |   - | ns    |
    // | tMDS   | Write Data Delay Time           |   - |  40 | ns    |
    // | tDHW   | Write Data Hold Time            |  10 |   - | ns    |
    //
    // Notes: (1) BE to High Impedance State is not testable but should be the
    //            same amount of time as BE to Valid Data
    //        (3) Since this is a static design, the maximum cycle time could
    //            be infinite
    //
    // Summary interpretations:
    //
    // Minimum Phi2 pulse width is high 62ns and low 63ns (tPWH, tPWL)
    //
    // Rising Phi2 edge triggers bus transfer:
    //  - BE must be asserted 45ns before rising Phi2 edge (tBVD + tDSR)
    //  - DIN must be valid 15ns before rising Phi2 edge (tDSR)
    //  - DIN must be held 10ns after rising Phi2 edge (tDHR)
    //  - BE must be held 10ns after rising Phi2 edge (tDHR, tDHW)
    //  - DOUT is valid within 40ns after rising Phi2 edge (tMDS)
    //  
    // Falling Phi2 edge advances CPU to next state:
    //  - Previous ADDR, RWB, DOUT held 10ns after falling Phi2 edge (tAH, tDHW)
    //  - Next ADDR, RWB, DOUT must be stable at least 40ns *before* the next
    //    rising Phi2 edge (tADS)
    //  - Write data becomes valid within 40ns *after* that rising edge (tMDS)
    //
    // Other (design-derived):
    //  - At maximum clock rate, RAM has 70ns between ADDR stabilizing and the
    //    beginning of the read setup time (tACC).
    //
    // Timing for W65C21 (PIA)
    // (See: https://www.westerndesigncenter.com/wdc/documentation/w65c21.pdf)
    //
    // | Symbol | Parameter                       | Min | Max | Units |
    // |--------|---------------------------------|-----|-----|-------|
    // | tCYC   | PHI2 Cycle                      |  70 |   - | ns    |
    // | tC     | PHI2 Pulse Width                |  35 |   - | ns    |
    // | trc    | PHI2 Rise Time                  |   - |   5 | ns    |
    // | tfc    | PHI2 Fall Time                  |   - |   5 | ns    |
    // | tACR   | Address Set-Up Time (read)      |   8 |   - | ns    |
    // | tCAR   | Address Hold Time (read)        |   0 |   - | ns    |
    // | tPCR   | Peripheral Data Setup Time      |  10 |   - | ns    |
    // | tCDR   | Data Bus Delay Time             |   - |  20 | ns    |
    // | tHR    | Data Bus Hold Time              |   5 |   - | ns    |
    // | tACW   | Address Set-Up Time (write)     |   8 |   - | ns    |
    // | tCAW   | Address Hold Time (write)       |   0 |   - | ns    |
    // | tDCW   | Data Bus Set-Up Time            |  10 |   - | ns    |
    // | tHW    | Data Bus Hold Time              |   5 |   - | ns    |
    // | tCPW   | Peripheral Data Delay Time      |   - |  20 | ns    |
    //
    // Summary interpretations:
    //
    // - ADDR, RWD, CS2B must be stable at least 8ns before rising Phi2 (tACR, tACW)
    //   and may be released 0ns after falling edge (tCAR, tCAW)
    // - Peripheral read data becomes valid within 20ns after rising Phi2 (tCDR)
    //   and should be held 5ns before falling edge (tHR)
    // - Peripheral write data must be set up 10ns before rising Phi2 (tDCW) and
    //   held 5ns after falling edge (tHW)
    // - Peripheral-side data can appear within 20ns after rising Phi2 (tCPW)
    //
    // Timing for SN74LVC4245A
    // (See: https://www.ti.com/lit/ds/symlink/sn74lvc4245a.pdf)
    //
    // Two SN74LVC4245A devices provide level shifting between the CPU and PIA/VIA:
    // - Address A0–A3: fixed direction from CPU→PIA/VIA (B→A), permanently enabled.
    // - Data D0–D7: bidirectional. DIR follows CPU RWB (read: A→B, write: B→A).
    //               /OE is asserted by the FPGA during decoded PIA/VIA accesses;
    //               otherwise the device is high‑Z to avoid contention.
    //
    // The timing values below bound propagation and enable/disable delays used when budgeting
    // setup/hold margins for CPU↔PIA/VIA transfers.
    //
    // | Symbol | From | To | Min | Max  | Units |
    // |--------|------|----|-----|------|-------|
    // | tPHL   | A    | B  |   1 |  6.3 | ns    |
    // | tPLH   | A    | B  |   1 |  6.7 | ns    |
    // | tPHL   | B    | A  |   1 |  6.1 | ns    |
    // | tPLH   | B    | A  |   1 |  5.0 | ns    |
    // | tPZL   | /OE  | A  |   1 |  9.0 | ns    |
    // | tPZH   | /OE  | A  |   1 | 10.0 | ns    |
    // | tPZL   | /OE  | B  |   1 | 10.3 | ns    |
    // | tPZH   | /OE  | B  |   1 |  9.8 | ns    |
    // | tPLZ   | /OE  | A  |   1 |  7.0 | ns    |
    // | tPHZ   | /OE  | B  |   1 |  5.8 | ns    |
    // | tPLZ   | /OE  | B  |   1 |  7.7 | ns    |
    // | tPHZ   | /OE  | B  |   1 |  7.8 | ns    |
    //
    // Summary interpretations:
    //
    // - Propagation: A→B up to 6.7ns (tPLH A→B); B→A up to 6.1ns (tPHL B→A). Budget 6.7ns worst‑case.
    // - Enable (/OE asserted): to B up to 10.3ns (tPZL /OE→B); to A up to 10.0ns (tPZH /OE→A).
    // - Disable (/OE deasserted): to B high‑Z up to 7.8ns (tPHZ /OE→B); to A high‑Z up to 7.0ns (tPLZ /OE→A). Budget 7.8ns worst‑case.
    // - Turn‑around: deassert /OE, wait ≥7.8ns to reach high‑Z, switch DIR, then re‑assert /OE to avoid contention.

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
               IOTX_t        = 11;  // IO Transceiver Worst-Case Delay (tPZL)

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
