// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

module timing (
    input  logic sys_clock_i,
    output logic clk16_en_o,
    output logic clk8_en_o,
    output logic cpu_be_o,
    output logic cpu_clock_o,
    output logic cpu_data_strobe_o,
    output logic cpu_wr_en_o,
    output logic load_sr1_o,
    output logic load_sr2_o,
    output logic [0:0] grant_o,
    output logic grant_valid_o
);
    initial begin
        clk16_en_o        = 1'b0;
        clk8_en_o         = 1'b0;
        cpu_be_o          = 1'b0;
        cpu_clock_o       = 1'b0;
        cpu_data_strobe_o = 1'b0;
        cpu_wr_en_o       = 1'b0;
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

    // Worst-case data hold time across read (tDHR) and write (tDHW) paths.
    localparam int CPU_tDHx = common_pkg::max2(CPU_tDHR, CPU_tDHW);

    // Worst-case IO transceiver enable delay across all /OE-to-output paths.
    localparam int IOTX_t = common_pkg::max4(IOTX_tPZL_A, IOTX_tPZH_A, IOTX_tPZL_B, IOTX_tPZH_B);

    // Worst-case IO transceiver propagation delay across all signal paths.
    localparam int IOTX_tPD = common_pkg::max4(IOTX_tPLH_AB, IOTX_tPHL_AB, IOTX_tPLH_BA, IOTX_tPHL_BA);

    localparam int CYCLES_BVD   = common_pkg::ns_to_cycles_with_trace_delay(CPU_tBVD),
                   CYCLES_IOTX  = common_pkg::ns_to_cycles_with_trace_delay(IOTX_t),
                   CYCLES_AA    = common_pkg::ns_to_cycles_with_trace_delay(SRAM_tAA),
                   CYCLES_MDS   = common_pkg::ns_to_cycles_with_trace_delay(CPU_tMDS),
                   CYCLES_CDR   = common_pkg::ns_to_cycles_with_trace_delay(IO_tCDR + IOTX_tPD),
                   CYCLES_PWH   = common_pkg::ns_to_cycles_with_trace_delay(CPU_tPWH),
                   CYCLES_DHX   = common_pkg::ns_to_cycles_with_trace_delay(CPU_tDHx);

    localparam int CPU_START = CPU_1 * 8,       // Each arbiter reservation is 8 cycles (125ns)
                   CPU_END   = CPU_START + 16;  // CPU controls bus for two consecutive slots (250ns)

    localparam int CPU_BE_START     = CPU_START,
                   CPU_PHI_START    = CPU_BE_START + CYCLES_BVD + CYCLES_IOTX,
                   CPU_PHI_END      = CPU_BE_END - CYCLES_DHX,      // Hold data for tDHx after end of PHI2
                   CPU_BE_END       = CPU_END - CYCLES_BVD;         // Allow tBVD for bus to return to High-Z

    // Data strobe fires at the earliest cycle when data from all bus drivers
    // is guaranteed to be valid:
    //
    //   RAM read:  address propagation (tBVD) + access time (tAA), each with trace delay
    //   CPU write: write data delay (tMDS) from rising PHI2, with trace delay
    //   IO read:   PIA/VIA data delay (tCDR) + transceiver propagation (tPD) from rising PHI2
    localparam int CPU_DATA_STROBE_RAM = CPU_BE_START + CYCLES_BVD + CYCLES_AA,
                   CPU_DATA_STROBE_CPU = CPU_PHI_START + CYCLES_MDS,
                   CPU_DATA_STROBE_IO  = CPU_PHI_START + CYCLES_CDR,
                   CPU_DATA_STROBE     = common_pkg::max3(CPU_DATA_STROBE_RAM, CPU_DATA_STROBE_CPU, CPU_DATA_STROBE_IO);

    // Write pulse: asserts at CPU_DATA_STROBE (CPU write data is guaranteed
    // valid) and deasserts one cycle before PHI2 falls. This ensures the
    // address and data buses are still stable when the SRAM latches on the
    // rising edge of WE_N.
    localparam int CPU_WR_START = CPU_DATA_STROBE,
                   CPU_WR_END   = CPU_PHI_END - 1;

    // Truncate to COUNTER_WIDTH bits
    localparam bit [COUNTER_WIDTH-1:0] CPU_BE_START_W    = COUNTER_WIDTH'(CPU_BE_START),
                                       CPU_PHI_START_W   = COUNTER_WIDTH'(CPU_PHI_START),
                                       CPU_DATA_STROBE_W = COUNTER_WIDTH'(CPU_DATA_STROBE),
                                       CPU_WR_END_W      = COUNTER_WIDTH'(CPU_WR_END),
                                       CPU_PHI_END_W     = COUNTER_WIDTH'(CPU_PHI_END),
                                       CPU_BE_END_W      = COUNTER_WIDTH'(CPU_BE_END);

    always_ff @(posedge sys_clock_i) begin
        cpu_data_strobe_o <= '0;

        unique case (cycle_count)
            CPU_BE_START_W: begin
                cpu_be_o <= 1'b1;
            end
            CPU_PHI_START_W: begin
                cpu_clock_o <= 1'b1;
            end
            CPU_DATA_STROBE_W: begin
                cpu_data_strobe_o <= 1'b1;
                cpu_wr_en_o <= 1'b1;
            end
            CPU_WR_END_W: begin
                cpu_wr_en_o <= 1'b0;
            end
            CPU_PHI_END_W: begin
                cpu_clock_o <= 1'b0;
            end
            CPU_BE_END_W: begin
                cpu_be_o <= 1'b0;
            end
            default: /* do nothing */;
        endcase
    end

    // synthesis off
    initial begin
        // Verify all cycle_count constants fit in COUNTER_WIDTH bits
        if (CPU_BE_START    != int'(CPU_BE_START_W))
            $fatal(1, "CPU_BE_START (%0d) does not fit in %0d bits", CPU_BE_START, COUNTER_WIDTH);
        if (CPU_PHI_START   != int'(CPU_PHI_START_W))
            $fatal(1, "CPU_PHI_START (%0d) does not fit in %0d bits", CPU_PHI_START, COUNTER_WIDTH);
        if (CPU_DATA_STROBE != int'(CPU_DATA_STROBE_W))
            $fatal(1, "CPU_DATA_STROBE (%0d) does not fit in %0d bits", CPU_DATA_STROBE, COUNTER_WIDTH);
        if (CPU_PHI_END     != int'(CPU_PHI_END_W))
            $fatal(1, "CPU_PHI_END (%0d) does not fit in %0d bits", CPU_PHI_END, COUNTER_WIDTH);
        if (CPU_WR_END      != int'(CPU_WR_END_W))
            $fatal(1, "CPU_WR_END (%0d) does not fit in %0d bits", CPU_WR_END, COUNTER_WIDTH);
        if (CPU_BE_END      != int'(CPU_BE_END_W))
            $fatal(1, "CPU_BE_END (%0d) does not fit in %0d bits", CPU_BE_END, COUNTER_WIDTH);

        // PHI2 pulse width high must meet minimum (tPWH)
        if ((CPU_PHI_END - CPU_PHI_START) * NS_PER_CYCLE < CPU_tPWH)
            $fatal(1, "PHI2 high time %.1fns violates tPWH >= %0dns",
                (CPU_PHI_END - CPU_PHI_START) * NS_PER_CYCLE, CPU_tPWH);

        // BE must be asserted long enough before PHI2 rises for bus to stabilize (tBVD + IO transceiver)
        if ((CPU_PHI_START - CPU_BE_START) * NS_PER_CYCLE < CPU_tBVD + IOTX_t)
            $fatal(1, "BE setup time %.1fns violates tBVD + tIOTX >= %0dns",
                (CPU_PHI_START - CPU_BE_START) * NS_PER_CYCLE, CPU_tBVD + IOTX_t);

        // Data must be held after PHI2 falls (tDHx)
        if ((CPU_BE_END - CPU_PHI_END) * NS_PER_CYCLE < CPU_tDHx)
            $fatal(1, "Data hold time %.1fns violates tDHx >= %0dns",
                (CPU_BE_END - CPU_PHI_END) * NS_PER_CYCLE, CPU_tDHx);

        // Bus must return to high-Z before next arbiter slot (tBVD)
        if ((CPU_END - CPU_BE_END) * NS_PER_CYCLE < CPU_tBVD)
            $fatal(1, "High-Z settling time %.1fns violates tBVD >= %0dns",
                (CPU_END - CPU_BE_END) * NS_PER_CYCLE, CPU_tBVD);

        // Data strobe must occur while PHI2 is high
        if (CPU_DATA_STROBE < CPU_PHI_START || CPU_DATA_STROBE >= CPU_PHI_END)
            $fatal(1, "CPU_DATA_STROBE (%0d) must be in [CPU_PHI_START (%0d), CPU_PHI_END (%0d))",
                CPU_DATA_STROBE, CPU_PHI_START, CPU_PHI_END);

        // RAM read setup: address out (tBVD + trace) then access time (tAA + trace)
        if ((CPU_DATA_STROBE - CPU_BE_START) * NS_PER_CYCLE < CPU_tBVD + SRAM_tAA + 2 * MAX_TRACE_DELAY_NS)
            $fatal(1, "RAM read setup %.1fns violates tBVD + tAA + 2*trace >= %0dns",
                (CPU_DATA_STROBE - CPU_BE_START) * NS_PER_CYCLE,
                CPU_tBVD + SRAM_tAA + 2 * MAX_TRACE_DELAY_NS);

        // CPU write setup: PHI2 to CPU (trace) + write data delay (tMDS) + data back (trace)
        if ((CPU_DATA_STROBE - CPU_PHI_START) * NS_PER_CYCLE < CPU_tMDS + 2 * MAX_TRACE_DELAY_NS)
            $fatal(1, "CPU write setup %.1fns violates tMDS + 2*trace >= %0dns",
                (CPU_DATA_STROBE - CPU_PHI_START) * NS_PER_CYCLE,
                CPU_tMDS + 2 * MAX_TRACE_DELAY_NS);

        // IO read setup: PHI2 to PIA (trace) + data delay (tCDR) + transceiver prop (tPD) + data back (trace)
        if ((CPU_DATA_STROBE - CPU_PHI_START) * NS_PER_CYCLE < IO_tCDR + IOTX_tPD + 2 * MAX_TRACE_DELAY_NS)
            $fatal(1, "IO read setup %.1fns violates tCDR + tPD + 2*trace >= %0dns",
                (CPU_DATA_STROBE - CPU_PHI_START) * NS_PER_CYCLE,
                IO_tCDR + IOTX_tPD + 2 * MAX_TRACE_DELAY_NS);

        // Write pulse width must meet minimum (tWP)
        if ((CPU_WR_END - CPU_WR_START) * NS_PER_CYCLE < SRAM_tWP)
            $fatal(1, "Write pulse %.1fns violates tWP >= %0dns",
                (CPU_WR_END - CPU_WR_START) * NS_PER_CYCLE, SRAM_tWP);

        // Write data must be valid tDW before end of write
        if ((CPU_WR_END - CPU_DATA_STROBE) * NS_PER_CYCLE < SRAM_tDW)
            $fatal(1, "Write data setup %.1fns violates tDW >= %0dns",
                (CPU_WR_END - CPU_DATA_STROBE) * NS_PER_CYCLE, SRAM_tDW);

        // Address must be valid tAW before end of write (address valid from BE start + tBVD)
        if ((CPU_WR_END - CPU_BE_START) * NS_PER_CYCLE < SRAM_tAW + CPU_tBVD)
            $fatal(1, "Write address setup %.1fns violates tAW + tBVD >= %0dns",
                (CPU_WR_END - CPU_BE_START) * NS_PER_CYCLE, SRAM_tAW + CPU_tBVD);

        // Write pulse must end before PHI2 falls
        if (CPU_WR_END >= CPU_PHI_END)
            $fatal(1, "CPU_WR_END (%0d) must be before CPU_PHI_END (%0d)",
                CPU_WR_END, CPU_PHI_END);

        // Sequential ordering of bus events
        if (!(CPU_BE_START < CPU_PHI_START
           && CPU_PHI_START < CPU_DATA_STROBE
           && CPU_DATA_STROBE < CPU_WR_END
           && CPU_WR_END < CPU_PHI_END
           && CPU_PHI_END < CPU_BE_END
           && CPU_BE_END < CPU_END))
            $fatal(1, "CPU timing events out of order: BE_START=%0d PHI_START=%0d DATA_STROBE=%0d WR_END=%0d PHI_END=%0d BE_END=%0d END=%0d",
                CPU_BE_START, CPU_PHI_START, CPU_DATA_STROBE, CPU_WR_END, CPU_PHI_END, CPU_BE_END, CPU_END);
    end
    // synthesis on
endmodule
