// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

module main (
    // FPGA
    input  logic sys_clock_i,   // 64 MHz clock (from PLL)

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
    input  logic cpu_sync_i,

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

    output logic io_oe_o,
    output logic pia1_cs_o,
    output logic pia2_cs_o,
    output logic via_cs_o,

    // Config from DIP switch
    input logic config_crt_i,       // Display type (0 = 12"/CRTC/20kHz, 1 = 9"/non-CRTC/15kHz)
    input logic config_keyboard_i,  // Keyboard type (0 = Business, 1 = Graphics)

    // Video
    input  logic graphic_i,         // VIA CA2 pin 39 -> Character ROM A10 (0 = graphics, 1 = text)
    output logic horiz_drive_o,     // Horizontal drive for native PET video
    output logic vert_drive_o,      // Vertical drive for native PET video
    output logic jiffy_clock_o,     // Triggers IRQ on falling edge (VIA CB1 pin 37)
    output logic video_o,

    // Audio
    input  logic diag_i,
    input  logic via_cb2_i,
    output logic audio_o,
    input  logic audio_det_i,       // Detects 3.5mm jack insertion (0 = not inserted, 1 = inserted)

    // SPI buses
    input  logic spi0_cs_ni,        // (CS)  Chip Select (active low)
    input  logic spi0_sck_i,        // (SCK) Serial Clock
    input  logic spi0_sd_i,         // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi0_sd_o,         // (SDO) Serial Data Out (FPGA -> MCU)
    
    input  logic spi1_cs_ni,        // (CS)  Chip Select (active low)
    input  logic spi1_sck_i,        // (SCK) Serial Clock
    input  logic spi1_sd_i,         // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi1_sd_o,         // (SDO) Serial Data Out (FPGA -> MCU)

    output logic spi_stall_o        // Flow control for SPI (0 = Ready, 1 = Busy)
);
    // WB Bus Declarations

    logic [WB_ADDR_WIDTH-1:0] wb_addr;
    logic [   DATA_WIDTH-1:0] wb_din;
    logic [   DATA_WIDTH-1:0] wb_dout;
    logic                     wb_we;
    logic                     wb_cycle;
    logic                     wb_strobe;
    logic                     wb_stall;
    logic                     wb_ack;

    logic ram_wb_sel;
    logic reg_wb_sel;
    logic kbd_wb_sel;
    logic crtc_wb_sel;
    logic bram_wb_sel;

    always_comb begin
        ram_wb_sel = 1'b0;
        reg_wb_sel = 1'b0;
        kbd_wb_sel = 1'b0;
        crtc_wb_sel = 1'b0;
        bram_wb_sel = 1'b0;

        unique casez (wb_addr)
            {WB_RAM_BASE,  {(WB_ADDR_WIDTH - $bits(WB_RAM_BASE)){1'b?}}}: ram_wb_sel = 1'b1;
            {WB_REG_BASE,  {(WB_ADDR_WIDTH - $bits(WB_REG_BASE)){1'b?}}}: reg_wb_sel = 1'b1;
            {WB_KBD_BASE,  {(WB_ADDR_WIDTH - $bits(WB_KBD_BASE)){1'b?}}}: kbd_wb_sel = 1'b1;
            {WB_CRTC_BASE, {(WB_ADDR_WIDTH - $bits(WB_CRTC_BASE)){1'b?}}}: crtc_wb_sel = 1'b1;
            {WB_BRAM_BASE, {(WB_ADDR_WIDTH - $bits(WB_BRAM_BASE)){1'b?}}}: bram_wb_sel = 1'b1;
            default: /* do nothing */ ;
        endcase
    end

    //
    // SPI <-> Wishbone Bridge
    //

    // TODO: At the moment, this is actually connected to the RP2040's SPI0 bus.  Rename or move?
    logic [WB_ADDR_WIDTH-1:0] spi1_addr;
    logic [   DATA_WIDTH-1:0] spi1_din;     // Peripheral -> SPI1 (WE=0)
    logic [   DATA_WIDTH-1:0] spi1_dout;    // SPI1 -> Peripheral (WE=1)
    logic                     spi1_we;
    logic                     spi1_cycle;
    logic                     spi1_strobe;
    logic                     spi1_stall;
    logic                     spi1_ack;

    spi1_controller spi1 (
        .wb_clock_i(sys_clock_i),
        .wbc_addr_o(spi1_addr),
        .wbc_data_i(spi1_din),
        .wbc_data_o(spi1_dout),
        .wbc_we_o(spi1_we),
        .wbc_cycle_o(spi1_cycle),
        .wbc_strobe_o(spi1_strobe),
        .wbc_stall_i(spi1_stall),
        .wbc_ack_i(spi1_ack),

        .spi_cs_ni(spi0_cs_ni),     // SPI CS_N
        .spi_sck_i(spi0_sck_i),     // SPI SCK
        .spi_sd_i (spi0_sd_i),      // SPI MCU TX  -> FPGA RX
        .spi_sd_o (spi0_sd_o),      // SPI FPGA TX -> MCU RX
        .spi_stall_o(spi_stall_o)   // Backpressure to MCU
    );

    // For now, IRQ is never driven by FPGA.
    assign cpu_irq_o   = 0;

    logic clk16_en;
    logic clk8_en;
    logic cpu_addr_strobe;  // Pulsed when cpu_addr_i is valid
    logic cpu_data_strobe;  // Pulsed when cpu_data_i is valid (when cpu_we_i=1 -> cpu is writing)
    logic cpu_data_en;      // Safe external write-data drive window
    logic cpu_hold_strobe;  // Pulsed when CPU tDHx has been met
    logic cpu_wr_en;
    logic load_sr1;
    logic load_sr2;
    logic [0:0] grant;
    logic grant_valid;

    // timing.sv's cpu_be_o drives internal RAM-address muxing (via
    // timing_cpu_be) and, in 6502 mode, the physical W65C02S's bus-enable.
    logic timing_cpu_be;
    logic soft_cpu_clock;

    // CPU select, driven by the register file below (REG_CPU_SEL).
    // Three options share one bitstream; switching does not reconfigure the
    // FPGA, so the CRT never loses sync. Each unselected core is held in reset.
    //   soft 6809  : SuperPET
    //   soft 6502  : virtual PET CPU (runs the boot menu; also fully simulatable)
    //   phys 6502  : the socketed W65C02S (may be unpopulated)
    logic [1:0] cpu_sel;
    wire cpu_is_6809     = (cpu_sel == CPU_SEL_SOFT_6809);
    wire cpu_is_soft6502 = (cpu_sel == CPU_SEL_SOFT_6502);

    // Both soft cores have no bus pins, so the FPGA drives the shared
    // address / R-W / write-data lines for them. The physical W65C02S drives
    // those itself, so the FPGA releases them when it is selected.
    wire cpu_soft        = cpu_is_6809 || cpu_is_soft6502;

    timing timing (
        .sys_clock_i(sys_clock_i),
        .clk16_en_o(clk16_en),
        .clk8_en_o(clk8_en),
        .cpu_be_o(timing_cpu_be),
        .cpu_clock_o(cpu_clock_o),
        .soft_cpu_clock_o(soft_cpu_clock),
        .cpu_addr_strobe_o(cpu_addr_strobe),
        .cpu_data_strobe_o(cpu_data_strobe),
        .cpu_data_en_o(cpu_data_en),
        .cpu_hold_strobe_o(cpu_hold_strobe),
        .cpu_wr_en_o(cpu_wr_en),
        .load_sr1_o(load_sr1),
        .load_sr2_o(load_sr2),
        .grant_o(grant),
        .grant_valid_o(grant_valid)
    );

    // Physical 6502 bus-enable. In 6809 mode the physical CPU stays socketed
    // but off the bus (BE low -> its address/data/R-W buffers are high-Z) so
    // the soft 6809 drives the shared bus instead. In 6502 mode the arbiter's
    // window (timing_cpu_be) enables the physical CPU to own the bus. See
    // hw/rev-b/.../CPU.kicad_sch: "When the FPGA drives the bus, it deasserts
    // BE ... to place the CPU's Address, Data, and R/~W buffers in high-Z."
    assign cpu_be_o = cpu_soft ? 1'b0 : timing_cpu_be;

    //
    // SuperPET soft 6809
    //
    // mc6809i is the vendored cavnex/mc6809 core (external/mc6809), wired
    // directly (not through the mc6809.v/mc6809e.v wrappers) because
    // timing_6809 generates E/Q itself rather than relying on the wrapper's
    // own MRDY-gated clock-phase generator.
    //

    logic [DATA_WIDTH-1:0] mc6809_dout;
    logic [CPU_ADDR_WIDTH-1:0] mc6809_addr;
    logic mc6809_rnw, mc6809_bs, mc6809_ba, mc6809_avma, mc6809_busy, mc6809_lic;
    logic [111:0] mc6809_regdata;

    logic cpu6809_be, cpu6809_e, cpu6809_q;
    logic cpu6809_addr_strobe, cpu6809_hold_strobe, cpu6809_wr_en;

    // cpu_irq_i is asynchronous to sys_clock_i (wire-ORed by the real
    // PIA1/PIA2/VIA chips), so double-flop it before mc6809i's nIRQ.
    logic [1:0] cpu_irq_sync = 2'b00;
    always_ff @(posedge sys_clock_i) begin
        cpu_irq_sync <= { cpu_irq_sync[0], cpu_irq_i };
    end

    // Super-OS/9 MMU state (driven by address_decoding below).
    logic superpet_flat;
    logic superpet_wp;
    logic superpet_firq_n;

    timing_6809 timing_6809 (
        .sys_clock_i(sys_clock_i),
        .cpu_be_i(timing_cpu_be),
        .cpu_clock_i(cpu_clock_o),
        .cpu_addr_strobe_i(cpu_addr_strobe),
        .cpu_hold_strobe_i(cpu_hold_strobe),
        .cpu_wr_en_i(cpu_wr_en),

        .cpu6809_be_o(cpu6809_be),
        .cpu6809_e_o(cpu6809_e),
        .cpu6809_q_o(cpu6809_q),
        .cpu6809_addr_strobe_o(cpu6809_addr_strobe),
        .cpu6809_hold_strobe_o(cpu6809_hold_strobe),
        .cpu6809_wr_en_o(cpu6809_wr_en)
    );

    // Capture read data before external PHI2 falls. The soft cores advance
    // later, after a peripheral may have released its read-data output.
    // Delay the consumer strobe so same-edge consumers see the new capture.
    // Both soft cores and the physical CPU share this captured-data boundary.
    // Video/WB keep the raw pins and raw strobe.
    logic [DATA_WIDTH-1:0] cpu_data_q = '0;
    logic [DATA_WIDTH-1:0] soft_cpu_data_q = '0;
    logic                  cpu_data_strobe_q = 1'b0;
    always_ff @(posedge sys_clock_i) begin
        if (cpu_data_strobe) cpu_data_q <= cpu_data_i;
        cpu_data_strobe_q <= cpu_data_strobe;
    end
    always_ff @(posedge cpu_data_strobe_q) begin
        soft_cpu_data_q <= cpu_data_i;
    end

    mc6809i mc6809 (
        .D(soft_cpu_data_q),
        .DOut(mc6809_dout),
        .ADDR(mc6809_addr),
        .RnW(mc6809_rnw),
        .E(cpu6809_e),
        .Q(cpu6809_q),
        .BS(mc6809_bs),
        .BA(mc6809_ba),
        .nIRQ(!cpu_irq_sync[1]),
        // No physical FIRQ line on this board (tied +5V on a real SuperPET
        // too); only the Super-OS/9 MMU pulses it, to wake the core out of
        // the flat-mode-exiting SYNC.
        .nFIRQ(superpet_firq_n),
        .nNMI(1'b1),
        .AVMA(mc6809_avma),
        .BUSY(mc6809_busy),
        .LIC(mc6809_lic),
        .nHALT(1'b1),
        // Held in reset whenever the physical 6502 owns the bus, so the soft
        // core is quiescent and never drives the shared bus in 6502 mode.
        .nRESET(!cpu_reset_i && cpu_is_6809),
        .nDMABREQ(1'b1),
        .RegData(mc6809_regdata)
    );

    // Soft MOS 6502 core (external/m6502) -- the virtual PET CPU. Its falling
    // clock edge follows external PHI2 by the PIA/VIA hold interval, so the
    // core naturally keeps its bus outputs stable while those devices latch.
    // Held in reset unless selected; outputs muxed into active_* below.
    logic [CPU_ADDR_WIDTH-1:0] m6502_addr;
    logic [    DATA_WIDTH-1:0] m6502_dout;
    logic                      m6502_rw;    // 1 = read, 0 = write
    logic                      m6502_sync;  // opcode-fetch marker (SYNC)

    cpu_6502 cpu_6502 (
        .i_clk(soft_cpu_clock),
        .o_phi1(),
        .o_phi2(),
        .i_reset_n(!cpu_reset_i && cpu_is_soft6502),
        .i_rdy(cpu_ready_o),
        .i_nmi_n(1'b1),
        .i_irq_n(!cpu_irq_sync[1]),   // real PET IRQ (PIA1 CB1 etc.)
        .i_so_n(1'b1),
        .o_sync(m6502_sync),
        .i_bus_data(soft_cpu_data_q),
        .o_bus_data(m6502_dout),
        .o_bus_addr(m6502_addr),
        .o_rw(m6502_rw),
        .i_debug_sel(3'b0),
        .o_debug_data()
    );

    // Muxed "active CPU" signals that every downstream consumer uses, so the
    // same decode/peripheral/bus logic serves whichever CPU owns the bus.
    // Address and R/W remain live because each soft core now advances only
    // after the external PHI2 hold interval.
    wire [CPU_ADDR_WIDTH-1:0] active_cpu_addr    = cpu_is_6809     ? mc6809_addr
                                                 : cpu_is_soft6502 ? m6502_addr
                                                 :                   cpu_addr_i;
    wire                      active_cpu_we      = cpu_is_6809     ? !mc6809_rnw
                                                 : cpu_is_soft6502 ? !m6502_rw
                                                 :                   cpu_we_i;
    wire [    DATA_WIDTH-1:0] active_cpu_dout    = cpu_is_6809 ? mc6809_dout : m6502_dout;
    wire                      active_be          = cpu_is_6809 ? cpu6809_be           : timing_cpu_be;
    wire                      active_addr_strobe = cpu_is_6809 ? cpu6809_addr_strobe  : cpu_addr_strobe;
    wire                      active_data_strobe = cpu_data_strobe_q;
    wire                      active_wr_en       = cpu_is_6809 ? cpu6809_wr_en        : cpu_wr_en;

    // Physical-6502 presence detector. When the physical CPU is selected
    // (cpu_soft=0) and running, a populated chip drives the shared address bus
    // (cpu_addr_i) with its fetch sequence. An empty socket leaves the bus
    // floating, so match all three fetches of a JMP-self loop rather than
    // any bus activity. Armed/cleared by a REG_CPU_SEL write.
    localparam logic [CPU_ADDR_WIDTH-1:0] PROBE_LOOP_ADDR = 16'h0400;

    logic       cpu_sel_wr;
    logic       phys_cpu_active = 1'b0;
    logic [2:0] phys_probe_seen = '0;
    always_ff @(posedge sys_clock_i) begin
        if (cpu_sel_wr) begin
            phys_cpu_active <= 1'b0;
            phys_probe_seen <= '0;
        end else if (!cpu_soft && active_data_strobe) begin
            if (cpu_addr_i == PROBE_LOOP_ADDR)           phys_probe_seen[0] <= 1'b1;
            if (cpu_addr_i == PROBE_LOOP_ADDR + 16'd1)   phys_probe_seen[1] <= 1'b1;
            if (cpu_addr_i == PROBE_LOOP_ADDR + 16'd2)   phys_probe_seen[2] <= 1'b1;
            if (&phys_probe_seen) phys_cpu_active <= 1'b1;
        end
    end

    wire cpu_wr_strobe = active_data_strobe && active_cpu_we;

    //
    // Wishbone <-> RAM Bridge
    //

    logic [DATA_WIDTH-1:0] ram_wb_din;
    logic                  ram_wb_stall;
    logic                  ram_wb_ack;

    logic [RAM_ADDR_WIDTH-1:0] ram_ctl_addr;    // Captured address for read/write cycle
    logic                      ram_ctl_oe;      // OE signal for read cycle
    logic                      ram_ctl_we;      // WE signal for write cycle
    logic [    DATA_WIDTH-1:0] ram_ctl_dout;    // FPGA -> RAM
    logic                      ram_ctl_doe;

    ram ram (
        .wb_clock_i(sys_clock_i),
        .wbp_addr_i(wb_addr),
        .wbp_data_i(wb_dout),
        .wbp_data_o(ram_wb_din),
        .wbp_we_i(wb_we),
        .wbp_cycle_i(wb_cycle),
        .wbp_strobe_i(wb_strobe),
        .wbp_stall_o(ram_wb_stall),
        .wbp_ack_o(ram_wb_ack),
        .wbp_sel_i(ram_wb_sel),

        .ram_oe_o(ram_ctl_oe),
        .ram_we_o(ram_ctl_we),
        .ram_addr_o(ram_ctl_addr),
        .ram_data_i(cpu_data_i),
        .ram_data_o(ram_ctl_dout),
        .ram_data_oe(ram_ctl_doe)
    );

    //
    // Register File
    //

    logic [DATA_WIDTH-1:0] reg_wb_din;
    logic reg_wb_stall;
    logic reg_wb_ack;
    logic video_col_80_mode;
    logic [11:10] video_ram_mask;

    logic reg_cpu_ready;
    logic bp_halted;
    logic [CPU_ADDR_WIDTH-1:0] bp_addr;
    logic bp_clear;

    register_file register_file (
        .wb_clock_i(sys_clock_i),
        .wbp_addr_i(wb_addr),
        .wbp_data_i(wb_dout),
        .wbp_data_o(reg_wb_din),
        .wbp_we_i(wb_we),
        .wbp_cycle_i(wb_cycle),
        .wbp_strobe_i(wb_strobe),
        .wbp_ack_o(reg_wb_ack),
        .wbp_stall_o(reg_wb_stall),
        .wbp_sel_i(reg_wb_sel),

        // Status register
        .video_graphic_i(graphic_i),
        .config_crt_i(config_crt_i),
        .config_keyboard_i(config_keyboard_i),
        .phys_cpu_active_i(phys_cpu_active),

        // CPU control register
        .cpu_ready_o(reg_cpu_ready),
        .cpu_reset_o(cpu_reset_o),
        .cpu_nmi_o(cpu_nmi_o),
        .cpu_sel_o(cpu_sel),
        .cpu_sel_wr_o(cpu_sel_wr),

        // Breakpoint
        .bp_halted_i(bp_halted),
        .bp_addr_i(bp_addr),
        .bp_clear_o(bp_clear),

        // Video control register
        .video_col_80_mode_o(video_col_80_mode),

        .video_ram_mask_o(video_ram_mask)
    );

    //
    // Breakpoint Detection
    //

    // cpu_sync_i floats with the socket empty, so soft cores use their own
    // fetch marker. The 6809 has none, so breakpoints are 6502-only.
    wire bp_sync = cpu_is_6809     ? 1'b0
                 : cpu_is_soft6502 ? m6502_sync
                 :                   cpu_sync_i;

    breakpoint breakpoint (
        .sys_clock_i(sys_clock_i),
        .cpu_sync_i(bp_sync),
        .cpu_data_i(cpu_data_q),
        .cpu_addr_i(active_cpu_addr),
        .cpu_data_strobe_i(active_data_strobe),
        .cpu_be_i(active_be),
        .cpu_ready_i(reg_cpu_ready),
        .clear_i(bp_clear),
        .cpu_ready_o(cpu_ready_o),
        .halted_o(bp_halted),
        .addr_o(bp_addr)
    );

    //
    // Address Decoding
    //

    logic ram_en;
    logic pia1_en;
    logic pia2_en;
    logic via_en;
    logic sid_en;
    logic io_en;
    logic crtc_en;
    logic unmapped;
    logic is_vram;
    logic is_readonly;
    logic decoded_a12;
    logic decoded_a13;
    logic decoded_a14;
    logic decoded_a15;
    logic decoded_a16;

    address_decoding address_decoding (
        .reset_i(cpu_reset_i),
        .sys_clock_i(sys_clock_i),
        
        .cpu_be_i(active_be),
        .cpu_wr_strobe_i(cpu_wr_strobe),
        .cpu_addr_i(active_cpu_addr),
        .cpu_data_i(cpu_data_q),
        .superpet_en_i(cpu_is_6809),   // SuperPET MMU only when the soft 6809 owns the bus

        .ram_en_o(ram_en),
        .pia1_en_o(pia1_en),
        .pia2_en_o(pia2_en),
        .via_en_o(via_en),
        .sid_en_o(sid_en),
        .io_en_o(io_en),
        .crtc_en_o(crtc_en),
        .unmapped_o(unmapped),
        .is_vram_o(is_vram),

        .is_readonly_o(is_readonly),
        .decoded_a12_o(decoded_a12),
        .decoded_a13_o(decoded_a13),
        .decoded_a14_o(decoded_a14),
        .decoded_a15_o(decoded_a15),
        .decoded_a16_o(decoded_a16),

        // Super-OS/9 MMU: SYNC bus state is BA=1/BS=0 on the 6809.
        .sync_i(mc6809_ba && !mc6809_bs),
        .superpet_flat_o(superpet_flat),
        .superpet_wp_o(superpet_wp),
        .superpet_firq_n_o(superpet_firq_n)
    );

    //
    // Video
    //

    logic [WB_ADDR_WIDTH-1:0] video_addr;    // Captured address for read
    logic [   DATA_WIDTH-1:0] video_din;     // Peripheral -> Video (WE=0)
    logic                     video_we;
    logic                     video_cycle;
    logic                     video_strobe;
    logic                     video_stall;
    logic                     video_ack;

    logic [   DATA_WIDTH-1:0] crtc_dout;     // CRTC -> CPU
    logic                     crtc_oe;

    logic [   DATA_WIDTH-1:0] crtc_wb_din;   // CRTC read back via Wishbone
    logic                     crtc_wb_stall;
    logic                     crtc_wb_ack;


    video video (
        // Wishbone controller used to fetch VRAM/VROM data
        .wb_clock_i(sys_clock_i),
        .wbc_addr_o(video_addr),
        .wbc_data_i(video_din),
        .wbc_we_o(video_we),
        .wbc_cycle_o(video_cycle),
        .wbc_strobe_o(video_strobe),
        .wbc_stall_i(video_stall),
        .wbc_ack_i(video_ack),

        // Wishbone peripheral for reading/writing CRTC registers
        .wbp_addr_i(wb_addr),
        .wbp_data_i(wb_dout),
        .wbp_data_o(crtc_wb_din),
        .wbp_we_i(wb_we),
        .wbp_cycle_i(wb_cycle),
        .wbp_strobe_i(wb_strobe),
        .wbp_stall_o(crtc_wb_stall),
        .wbp_ack_o(crtc_wb_ack),
        .wbp_sel_i(crtc_wb_sel),

        // Video timing
        .clk8_en_i(clk8_en),                // 8 MHz pixel clock for 40 column mode
        .clk16_en_i(clk16_en),              // 16 MHz pixel clock for 80 column mode

        .config_crt_i(config_crt_i),        // Controls polarity of video signals (0 = 12"/CRTC, 1 = 9"/non-CRTC)

        .cpu_reset_i(cpu_reset_i),
        .crtc_clk_en_i(cpu_data_strobe),      // 1 MHz clock enable for 'sys_clock_i'
        .crtc_cs_i(crtc_en),                // Asserted by address decoding when 'active_cpu_addr' is in CRTC range
        .crtc_rs_i(active_cpu_addr[0]),     // Register select (0 = write address/read status, 1 = read addressed register)
        .crtc_we_i(active_cpu_we),          // Direction of data transfers (0 = reading from CRTC, 1 = writing to CRTC)
        .crtc_data_i(cpu_data_i),           // CPU -> CRTC
        .crtc_data_o(crtc_dout),            // CRTC -> CPU
        .crtc_data_oe(crtc_oe),             // Asserted when CPU is reading from CRTC

        // Dot Gen
        .load_sr1_i(load_sr1),
        .load_sr2_i(load_sr2),
        .col_80_mode_i(video_col_80_mode),  // 0 = 40 column mode, 1 = 80 column mode
        .graphic_i(graphic_i),
        .h_sync_o(horiz_drive_o),
        .v_sync_o(vert_drive_o),
        .video_o(video_o)
    );

    // For now, always use vert_drive for jiffy interrupt generated by PIA1 CB1.
    assign jiffy_clock_o = vert_drive_o;

    //
    // Audio
    //
    audio audio (
        .reset_i(cpu_reset_i),
        .sys_clock_i(sys_clock_i),
        .clk1_en_i(load_sr1),
        .cpu_wr_strobe_i(cpu_wr_strobe),
        .sid_en_i(sid_en),
        .addr_i(active_cpu_addr[SID_ADDR_REG_WIDTH-1:0]),
        .data_i(cpu_data_q),
        .data_o(),                 // TODO: Read back from SID?
        .diag_i(diag_i),
        .via_cb2_i(via_cb2_i),
        .audio_o(audio_o)
    );

    //
    // BRAM (Character ROM)
    //

    logic [DATA_WIDTH-1:0] bram_wb_din;
    logic                  bram_wb_stall;
    logic                  bram_wb_ack;

    bram #(
        .DATA_DEPTH(4096)   // 4KB for character ROM (2 character sets x 2KB each)
    ) bram (
        .wb_clock_i(sys_clock_i),
        .wbp_addr_i(wb_addr),
        .wbp_data_i(wb_dout),
        .wbp_data_o(bram_wb_din),
        .wbp_we_i(wb_we),
        .wbp_cycle_i(wb_cycle),
        .wbp_strobe_i(wb_strobe),
        .wbp_stall_o(bram_wb_stall),
        .wbp_ack_o(bram_wb_ack),
        .wbp_sel_i(bram_wb_sel)
    );

    //
    // USB Keyboard
    //

    logic [DATA_WIDTH-1:0] kbd_wb_din;
    logic                  kbd_wb_stall;
    logic                  kbd_wb_ack;

    logic [DATA_WIDTH-1:0] io_dout;
    logic                  io_doe;

    keyboard keyboard (
        .wb_clock_i(sys_clock_i),
        .wbp_addr_i(wb_addr),
        .wbp_data_i(wb_dout),
        .wbp_data_o(kbd_wb_din),
        .wbp_we_i(wb_we),
        .wbp_cycle_i(wb_cycle),
        .wbp_strobe_i(wb_strobe),
        .wbp_stall_o(kbd_wb_stall),
        .wbp_ack_o(kbd_wb_ack),
        .wbp_sel_i(kbd_wb_sel),
        
        .cpu_be_i(active_be),
        .cpu_data_strobe_i(active_data_strobe),
        .cpu_data_i(cpu_data_q),
        .cpu_data_o(io_dout),
        .cpu_data_oe(io_doe),
        .cpu_we_i(active_cpu_we),

        .pia1_cs_i(pia1_en),
        .pia1_rs_i(active_cpu_addr[PIA_RS_WIDTH-1:0])
    );

    //
    // Wishbone
    //

    // Many controllers -> one bus
    wbc_mux #(
        .COUNT(2)
    ) wbc_mux (
        .wb_clock_i(sys_clock_i),

        // Wishbone controllers to mux
        .wbc_cycle_i({ spi1_cycle, video_cycle }),
        .wbc_strobe_i({ spi1_strobe, video_strobe }),
        .wbc_addr_i({ spi1_addr, video_addr }),
        .wbc_din_o({ spi1_din, video_din }),
        .wbc_dout_i({ spi1_dout, 8'hxx }), // Video has no data out
        .wbc_we_i({ spi1_we, video_we }),
        .wbc_stall_o({ spi1_stall, video_stall }),
        .wbc_ack_o({ spi1_ack, video_ack }),

        // Wishbone bus
        .wb_addr_o(wb_addr),
        .wb_din_i(wb_din),
        .wb_dout_o(wb_dout),
        .wb_we_o(wb_we),
        .wb_cycle_o(wb_cycle),
        .wb_strobe_o(wb_strobe),
        .wb_stall_i(wb_stall),
        .wb_ack_i(wb_ack),

        // Control signals
        .wbc_grant_i(grant),
        .wbc_grant_valid_i(grant_valid)
    );

    // One bus -> many peripherals
    wbp_mux #(
        .COUNT(5)
    ) wbp_mux (
        .wbp_sel_i({ ram_wb_sel, reg_wb_sel, kbd_wb_sel, crtc_wb_sel, bram_wb_sel }),

        // Wishbone Bus
        .wb_din_o(wb_din),
        .wb_stall_o(wb_stall),
        .wb_ack_o(wb_ack),

        // Wishbone peripherals to mux
        .wbp_din_i({ ram_wb_din, reg_wb_din, kbd_wb_din, crtc_wb_din, bram_wb_din }),
        .wbp_stall_i({ ram_wb_stall, reg_wb_stall, kbd_wb_stall, crtc_wb_stall, bram_wb_stall }),
        .wbp_ack_i({ ram_wb_ack, reg_wb_ack, kbd_wb_ack, crtc_wb_ack, bram_wb_ack })
    );

    //
    // System Bus
    //

    logic [DATA_WIDTH-1:0] open_bus_dout;
    logic                  open_bus_oe;

    open_bus open_bus (
        .sys_clock_i(sys_clock_i),
        .cpu_data_strobe_i(active_data_strobe),
        .cpu_data_i(cpu_data_q),
        .unmapped_i(unmapped),
        .cpu_be_i(active_be),
        .cpu_we_i(active_cpu_we),
        .data_o(open_bus_dout),
        .data_oe(open_bus_oe)
    );

    // Soft cores have no bus pins, so the FPGA drives their write data. The mux
    // selects it for the whole BE window, which gives the registered pad output
    // time to settle before the enable below opens.
    wire cpu_write_sel = cpu_soft && active_be && active_cpu_we;

    // Mirror a physical CPU's write-data window: high-Z until the data-valid
    // point (tMDS past rising PHI2), then held past WE's rising edge until BE
    // drops, so releasing the driver never races the SRAM latch.
    wire cpu_driving_data_bus = cpu_write_sel && cpu_data_en;

    logic [DATA_WIDTH-1:0] cpu_data_mux_out;
    logic                  cpu_data_mux_oe;

    //
    // SuperPET 6702 security dongle at $EFE0 (Waterloo startup checks it)
    //
    logic [DATA_WIDTH-1:0] dongle_dout;
    logic                  dongle_doe;

    dongle6702 dongle6702 (
        .sys_clock_i(sys_clock_i),
        .reset_i(cpu_reset_i),
        .cpu_be_i(active_be),
        .cpu_data_strobe_i(active_data_strobe),
        .cpu_addr_i(active_cpu_addr),
        .cpu_data_i(cpu_data_q),
        .cpu_data_o(dongle_dout),
        .cpu_data_oe(dongle_doe),
        .cpu_we_i(active_cpu_we),
        // The dongle exists only in 6809 mode, and flat mode maps $EFE0
        // as RAM, so gate it off in both cases.
        .enable_i(cpu_is_6809 && !superpet_flat)
    );

    // Many controllers -> System data bus
    cpu_data_mux #(
        .COUNT(6)
    ) cpu_data_mux (
        .data_i({ open_bus_dout, ram_ctl_dout, crtc_dout, io_dout, dongle_dout, active_cpu_dout }),
        .oe_i({ open_bus_oe & !dongle_doe, ram_ctl_doe, crtc_oe & active_be, io_doe & active_be & !active_cpu_we, dongle_doe & active_be & !active_cpu_we, cpu_write_sel }),
        .data_o(cpu_data_mux_out),
        .oe_o(cpu_data_mux_oe)
    );

    assign cpu_data_oe = cpu_write_sel ? cpu_data_en : cpu_data_mux_oe;

    // Register the pad output; the mux select cone can miss timing. Sources
    // are stable for multiple cycles, so the delay is harmless. cpu_data_oe
    // stays combinational (false-pathed in the SDC).
    always_ff @(posedge sys_clock_i) begin
        cpu_data_o <= cpu_data_mux_out;
    end

    wire cpu_rd_en = active_be && !active_cpu_we;

    // A fabric peripheral claiming the bus (dongle data_oe) must also
    // suppress the external I/O chip selects, or the FPGA and a real
    // PIA/VIA drive the data bus in the same cycle. io_doe (the keyboard
    // intercept) only shadows PIA1, so pia2/via omit it.
    assign io_oe_o   = io_en   && active_be && !io_doe && !dongle_doe;
    assign pia1_cs_o = pia1_en && active_be && !io_doe && !dongle_doe;
    assign pia2_cs_o = pia2_en && active_be && !dongle_doe;
    assign via_cs_o  =  via_en && active_be && !dongle_doe;

    assign ram_oe_o         = (cpu_rd_en && ram_en) || ram_ctl_oe;
    assign ram_we_o         = (active_wr_en && active_cpu_we && ram_en && !is_readonly
                                && !superpet_wp) || ram_ctl_we;

    // A soft core has no bus pins, so the FPGA drives the address during its
    // window and during every WB<->RAM bridge slot. With the physical 6502
    // selected, the FPGA drives only the bridge slots.
    assign cpu_addr_oe      = cpu_soft || !active_be;

    // SRAM A12-A14 have no dedicated FPGA pins -- they are wired to the shared
    // bus. With the soft 6809 the FPGA drives the bus anyway, so 16-bank
    // $9000-$9FFF switching needs no hardware change: splice the bank-translated
    // a12-a14 (identity outside the window) into the bus address during the CPU
    // window. I/O is unaffected -- its selects come from active_cpu_addr and A0-A1.
    assign cpu_addr_o       = active_be
        ? { active_cpu_addr[15], decoded_a14, decoded_a13, decoded_a12, active_cpu_addr[11:0] }
        : ram_ctl_addr[15:0];

    // R/W feeds the U8 level shifter and the PIA/VIA R/W inputs with no
    // pull-up on the net (see MAGIC.kicad_sch), so it must be driven
    // whenever a soft core owns the bus -- and released for the physical
    // CPU, which drives its own R/W pin.
    assign cpu_we_oe        = cpu_soft && active_be;
    assign cpu_we_o         = active_cpu_we;

    wire ram_addr_a10_mask = !is_vram | video_ram_mask[10];
    wire ram_addr_a11_mask = !is_vram | video_ram_mask[11];

    // When the CPU is driving the bus, apply masks to RAM A10/A11 to wrap video memory.
    assign ram_addr_a10_o = active_be
        ? active_cpu_addr[10] & ram_addr_a10_mask
        : cpu_addr_o[10];

    assign ram_addr_a11_o = active_be
        ? active_cpu_addr[11] & ram_addr_a11_mask
        : cpu_addr_o[11];

    // When the CPU is driving the bus, the control register at $FFF0 controls
    // the memory mapping for the upper 64k expansion.
    assign ram_addr_a15_o = active_be ? decoded_a15 : cpu_addr_o[15];
    assign ram_addr_a16_o = active_be ? decoded_a16 : ram_ctl_addr[16];

    // synthesis off

    // At most one source may drive the CPU data bus at a time:
    //   ram_oe_o      - External SRAM (output enabled)
    //   io_oe_o       - External IO chips (PIA1, PIA2, VIA) when selected and CPU is reading
    //   cpu_data_oe   - FPGA (keyboard intercept, CRTC, WB-RAM bridge, and a soft
    //                   core's own write data, which the FPGA must drive since the
    //                   core has no bus pins of its own)
    wire ram_driving_data_bus = ram_oe_o;
    wire io_driving_data_bus = io_oe_o && !active_cpu_we;
    wire fpga_driving_data_bus = cpu_data_oe;

    // Socketed CPU drives write data externally; cpu_driving_data_bus is soft-core only.
    wire phys_cpu_driving_data_bus = !cpu_soft && active_be && active_cpu_we;

    wire [2:0] dbg_data_bus_drivers = {ram_driving_data_bus, io_driving_data_bus, fpga_driving_data_bus};
    wire [1:0] dbg_ram_oe_we = {ram_oe_o, ram_we_o};

    // PIA/VIA latch on PHI2's falling edge, and therefore require ADDR, DATA, and RW
    // to be held stable for a short time after PHI2 drops.
    logic [CPU_ADDR_WIDTH-1:0] dbg_prev_cpu_addr;
    logic                      dbg_prev_cpu_we;
    logic [    DATA_WIDTH-1:0] dbg_prev_cpu_data;
    logic                      dbg_prev_be = 1'b0;
    logic                      dbg_prev_data_oe = 1'b0;
    always_ff @(posedge sys_clock_i) begin
        // The delayed soft-core falling edges keep ADDR and R/W stable for the
        // entire BE window.
        if (cpu_soft && active_be && dbg_prev_be) begin
            assert (active_cpu_addr === dbg_prev_cpu_addr)
                else $fatal(1, "active_cpu_addr changed %h -> %h while BE asserted", dbg_prev_cpu_addr, active_cpu_addr);
            assert (active_cpu_we === dbg_prev_cpu_we)
                else $fatal(1, "active_cpu_we changed %b -> %b while BE asserted", dbg_prev_cpu_we, active_cpu_we);
        end

        // We assert that the FPGA's write-data output is stable while the CPU is
        // driving the bus.
        if (cpu_driving_data_bus && dbg_prev_data_oe) begin
            assert (cpu_data_o === dbg_prev_cpu_data)
                else $fatal(1, "cpu_data_o changed %h -> %h while driving write data", dbg_prev_cpu_data, cpu_data_o);
        end

        dbg_prev_be       <= active_be;
        dbg_prev_cpu_addr <= active_cpu_addr;
        dbg_prev_cpu_we   <= active_cpu_we;
        dbg_prev_data_oe  <= cpu_driving_data_bus;
        dbg_prev_cpu_data <= cpu_data_o;
    end

    always_ff @(posedge sys_clock_i or negedge sys_clock_i) begin
        assert(!active_be || !ram_wb_stall) else $fatal(1, "WB<->RAM bridge must be stalled when CPU is driving bus");
        assert(!active_be || !ram_ctl_oe)   else $fatal(1, "WB<->RAM bridge must not assert OE when CPU is driving bus");
        assert(!active_be || !ram_ctl_we)   else $fatal(1, "WB<->RAM bridge must not assert WE when CPU is driving bus");
        assert(!active_be || !ram_wb_ack)   else $fatal(1, "WB<->RAM bridge must not assert ACK when CPU is driving bus");
        assert(!io_oe_o  ||  active_be)     else $fatal(1, "IO must not be active unless CPU is driving bus");
        assert(!io_oe_o  || !ram_we_o)      else $fatal(1, "IO and RAM_WE must not be active at same time");

        assert(!ram_we_o || ram_ctl_doe
               || ((cpu_driving_data_bus || phys_cpu_driving_data_bus) && active_wr_en))
            else $fatal(1, "RAM_WE asserted but valid data not driven to bus");

        assert($onehot0(dbg_data_bus_drivers)) else $fatal(1, "Multiple drivers on CPU data bus: {ram, io, fpga}=%b", dbg_data_bus_drivers);
        assert($onehot0(dbg_ram_oe_we)) else $fatal(1, "RAM must be reading or writing, not both: {ram_oe, ram_we}=%b", dbg_ram_oe_we);
    end
    // synthesis on
endmodule
