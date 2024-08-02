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

module system (
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_clock_i,
    input  logic [WB_ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [   DATA_WIDTH-1:0] wb_data_i,
    output logic [   DATA_WIDTH-1:0] wb_data_o,
    input  logic                     wb_we_i,
    input  logic                     wb_cycle_i,
    input  logic                     wb_strobe_i,
    output logic                     wb_stall_o,
    output logic                     wb_ack_o,

    // CPU
    input  logic cpu_reset_i,
    output logic cpu_reset_o,
    output logic cpu_be_o,
    output logic cpu_ready_o,
    output logic cpu_clock_o,
    input  logic cpu_irq_i,
    output logic cpu_irq_o,
    input  logic cpu_nmi_i,
    output logic cpu_nmi_o,

    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    output logic [CPU_ADDR_WIDTH-1:0] cpu_addr_o,
    output logic                      cpu_addr_oe,

    input  logic [DATA_WIDTH-1:0] cpu_data_i,
    output logic [DATA_WIDTH-1:0] cpu_data_o,
    output logic                  cpu_data_oe,

    input  logic cpu_we_i,
    output logic cpu_we_o,
    output logic cpu_we_oe,

    // RAM
    output logic ram_addr_a10_o,
    output logic ram_addr_a11_o,
    output logic ram_addr_a15_o,
    output logic ram_addr_a16_o,
    output logic ram_oe_o,
    output logic ram_we_o,

    // IO
    output logic io_oe_o,
    output logic pia1_cs_o,
    output logic pia2_cs_o,
    output logic via_cs_o
);
    // For now, outgoing CPU control signals are constant.
    assign cpu_ready_o = 1;
    assign cpu_reset_o = 0;
    assign cpu_irq_o   = 0;
    assign cpu_nmi_o   = 0;

    initial begin
        cpu_clock_o = '0;

        // Avoid contention on startup
        cpu_be_o    = '0;
        cpu_we_o    = '0;
        cpu_we_oe   = '0;
    end

    logic [5:0] clock_counter = '0;

    always_ff @(posedge wb_clock_i) begin
        clock_counter <= clock_counter + 1'b1;
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
        // Note: We conservatively add 1 cycle (15.625ns) to account for propagation delay.
        return 6'(int'($ceil(time_ns / (1000.0 / SYS_CLOCK_MHZ)) + 1));
    endfunction

    // Maximum number of 'wb_clock_i' cycles required to complete an in-progress
    // wishbone transaction with RAM.
    localparam MAX_WB_CYCLES = 5,
               CPU_tBVD      = 30,  // CPU BE to Valid Data (tBVD)
               CPU_tPWH      = 62,  // CPU Clock Pulse Width High (tPWH)
               CPU_tDSR      = 15,  // CPU Data Setup Time (tDSR)
               CPU_tDHx      = 10,  // CPU Data Hold Time (tDHR, tDHW)
               RAM_tAA       = 10,  // RAM Address Access Time (tAA)
               IOTX_t        = 11;  // IO Transciever Worst-Case Delay (tPZL)

    localparam bit [5:0] WB_READY_END     = 0,
                         CPU_BE_START     = MAX_WB_CYCLES,                                                          // Assert BE at beginning of CPU grant
                         BUS_VALID_START  = CPU_BE_START    + ns_to_cycles(CPU_tBVD),                               // ADDR, RWB, DOUT now valid
                         IO_VALID_START   = BUS_VALID_START + ns_to_cycles(IOTX_t > RAM_tAA ? IOTX_t : RAM_tAA),
                         CPU_PHI_START    = IO_VALID_START  + ns_to_cycles(CPU_tDSR),               // Wait until DIN is valid and CPU setup time met before starting transaction
                         CPU_PHI_END      = CPU_PHI_START   + ns_to_cycles(CPU_tPWH),               // Hold Phi2 high for the required time
                         CPU_BE_END       = CPU_PHI_END     + ns_to_cycles(CPU_tDHx),               // Hold data for required time before beginning transition to high-z
                         WB_BE_START      = CPU_BE_END      + ns_to_cycles(CPU_tBVD),               // Wait until CPU transitions to high-Z before granting control to wishbone
                         WB_READY_START   = WB_BE_START     + 1;                                    // WB now driving ADDR.  Begin accepting new transactions

    logic wb_ready   = 0;
    logic io_valid   = 0;
    logic bus_valid  = 0;
    logic cpu_ram_we = 0;
    logic cpu_ram_oe = 0;
    logic cpu_io_oe  = 0;
    logic wb_addr_oe = 0;

    always_ff @(posedge wb_clock_i) begin
        case (clock_counter)
            WB_READY_END: begin         // Stop admitting new Wishbone requests
                wb_ready    <= 0;
            end
            CPU_BE_START: begin         // In-progress transactions have drained.
                cpu_be_o    <= 1;       // CPU begins transition out of high-z state.
                wb_addr_oe  <= 0;
            end
            BUS_VALID_START: begin      // CPU now driving ADDR, WE, and DOUT.
                bus_valid   <= 1;       // Address decoding done.
                cpu_ram_oe  <= ram_en && !cpu_we_i;
                cpu_io_oe   <= io_en;
            end
            IO_VALID_START: begin       // RAM access or IO transceiver delay met.
                io_valid    <= 1;
            end
            CPU_PHI_START: begin        // CPU setup time met.
                cpu_clock_o <= 1;
                cpu_ram_we  <= ram_en && cpu_we_i;
            end
            CPU_PHI_END: begin          // CPU mimimum clock pulse width met.
                cpu_clock_o <= 0;
                cpu_ram_we  <= 0;
            end
            CPU_BE_END: begin           // CPU and I/O hold times met.  Start transition to high-Z.
                cpu_be_o    <= 0;
                bus_valid   <= 0;
                io_valid    <= 0;
                cpu_ram_oe  <= 0;
                cpu_io_oe   <= 0;
            end
            WB_BE_START: begin          // FPGA begins transition to high-Z.
                wb_addr_oe  <= 1;
            end
            WB_READY_START: begin       // Begin accepting new Wishbone requests
                wb_ready    <= 1;
            end
            default: begin
            end
        endcase
    end

    // Wishbone interface should only accept transactions when 'wb_ready' is high.
    wire ram_ctl_cycle  = wb_ready & wb_cycle_i;
    wire ram_ctl_strobe = wb_ready & wb_strobe_i;
    
    logic ram_ctl_stall;
    assign wb_stall_o   = ~wb_ready | ram_ctl_stall;

    logic wb_ram_oe;
    logic wb_ram_we;
    logic [RAM_ADDR_WIDTH-1:0] wb_ram_addr;

    ram ram (
        .wb_clock_i(wb_clock_i),
        .wb_addr_i(wb_addr_i[16:0]),
        .wb_data_i(wb_data_i),
        .wb_data_o(wb_data_o),
        .wb_we_i(wb_we_i),

        .wb_cycle_i(ram_ctl_cycle),
        .wb_strobe_i(ram_ctl_strobe),
        .wb_stall_o(ram_ctl_stall),
        .wb_ack_o(wb_ack_o),

        .ram_oe_o(wb_ram_oe),
        .ram_we_o(wb_ram_we),
        .ram_addr_o(wb_ram_addr),
        .ram_data_i(cpu_data_i),
        .ram_data_o(cpu_data_o),
        .ram_data_oe(cpu_data_oe)
    );

    //
    // Address Decoding
    //

    logic ram_en;
    logic pia1_en;
    logic pia2_en;
    logic via_en;
    logic io_en;

    address_decoding address_decoding(
        .addr_i(cpu_addr_i),
        .ram_en_o(ram_en),
        .pia1_en_o(pia1_en),
        .pia2_en_o(pia2_en),
        .via_en_o(via_en),
        .io_en_o(io_en)
    );

    assign io_oe_o   = cpu_io_oe;
    assign pia1_cs_o = pia1_en && io_valid && cpu_io_oe;
    assign pia2_cs_o = pia2_en && io_valid && cpu_io_oe;
    assign via_cs_o  =  via_en && io_valid && cpu_io_oe;

    assign ram_oe_o         = (cpu_ram_oe && !cpu_we_i) || wb_ram_oe;
    assign ram_we_o         = cpu_ram_we || wb_ram_we;

    assign cpu_addr_oe      = wb_addr_oe;
    assign cpu_addr_o       = wb_ram_addr[15:0];

    assign ram_addr_a10_o   = cpu_be_o ? cpu_addr_i[10] : cpu_addr_o[10];
    assign ram_addr_a11_o   = cpu_be_o ? cpu_addr_i[11] : cpu_addr_o[11];
    assign ram_addr_a15_o   = cpu_be_o ? cpu_addr_i[15] : cpu_addr_o[15];
    assign ram_addr_a16_o   = cpu_be_o ? 1'b0 : wb_ram_addr[16];

    // synthesis off
    always_ff @(posedge wb_clock_i or negedge wb_clock_i) begin
        assert(!cpu_be_o || !ram_ctl_stall) else $fatal(1, "cpu_ram_oe and ram_ctl_stall cannot be asserted simultaneously");
        assert(!cpu_be_o || !wb_ram_oe) else $fatal(1, "cpu_ram_oe and wb_ram_oe cannot be asserted simultaneously");
        assert(!io_oe_o  || !wb_ram_oe) else $fatal(1, "io_oe_o and wb_ram_oe cannot be asserted simultaneously");
    end
    // synthesis on
endmodule
