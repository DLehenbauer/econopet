// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

// Top module encapsulates/normalizes platform and hardware quirks before connecting
// signals to the main module.  This includes:
//
//  - Normalizing signals to be active high signals.
//  - Combining OE signals into a single bus-wide OE signal.
//  - Drive currently unused signals to a known state.
module top #(
    parameter integer unsigned WB_ADDR_WIDTH = 20,
    parameter integer unsigned RAM_ADDR_WIDTH = 17,
    parameter integer unsigned CPU_ADDR_WIDTH = 16,
    parameter integer unsigned DATA_WIDTH = 8
) (
    // FPGA
    input  logic sys_clock_i,   // 64 MHz clock (from PLL)
    output logic status_no,     // Red NSTATUS LED (0 = On, 1 = Off)

    // CPU
    input  logic cpu_reset_n_i,
    output logic cpu_reset_n_o,
    output logic cpu_reset_n_oe,

    output logic cpu_be_o,
    output logic cpu_clock_o,
    output logic cpu_ready_o,

    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    output logic [CPU_ADDR_WIDTH-1:0] cpu_addr_o,
    output logic [CPU_ADDR_WIDTH-1:0] cpu_addr_oe,

    input  logic [DATA_WIDTH-1:0] cpu_data_i,
    output logic [DATA_WIDTH-1:0] cpu_data_o,
    output logic [DATA_WIDTH-1:0] cpu_data_oe,

    input  logic cpu_we_n_i,
    output logic cpu_we_n_o,
    output logic cpu_we_n_oe,

    input  logic cpu_irq_n_i,
    output logic cpu_irq_n_o,
    output logic cpu_irq_n_oe,

    input  logic cpu_nmi_n_i,
    output logic cpu_nmi_n_o,
    output logic cpu_nmi_n_oe,

    input  logic cpu_sync_i,        // Asserted 70ns after falling PHI2 if next cycle fetches an opcode.

    // RAM
    output logic ram_addr_a10_o,
    output logic ram_addr_a11_o,
    output logic ram_addr_a15_o,
    output logic ram_addr_a16_o,
    output logic ram_oe_n_o,
    output logic ram_we_n_o,

    // IO
    output logic io_oe_n_o,
    output logic pia1_cs_n_o,           // (CS2B)
    output logic pia2_cs_n_o,           // (CS2B)
    output logic via_cs_n_o,            // (CS2B)

    // SPI buses
    input  logic spi0_cs_ni,            // (CS)  Chip Select (active low)
    input  logic spi0_sck_i,            // (SCK) Serial Clock
    input  logic spi0_sd_i,             // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi0_sd_o,             // (SDO) Serial Data Out (FPGA -> MCU)
    
    input  logic spi1_cs_ni,        // (CS)  Chip Select (active low)
    input  logic spi1_sck_i,        // (SCK) Serial Clock
    input  logic spi1_sd_i,         // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi1_sd_o,         // (SDO) Serial Data Out (FPGA -> MCU)

    output logic spi_stall_o,       // Flow control for SPI (0 = Ready, 1 = Busy)

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
    input  logic audio_det_n_i,     // Detects 3.5mm jack insertion (0 = inserted, 1 = not inserted)

    // PMOD
    input  logic [8:1] pmod1_i,
    output logic [8:1] pmod1_o,
    output logic [8:1] pmod1_oe,

    input  logic [8:1] pmod2_i,
    output logic [8:1] pmod2_o,
    output logic [8:1] pmod2_oe,

    // Spare pins
    input  logic sp1_i,
    output logic sp1_o,
    output logic sp1_oe,

    input  logic sp2_i,
    output logic sp2_o,
    output logic sp2_oe,

    input  logic sp3_i,
    output logic sp3_o,
    output logic sp3_oe,

    input  logic sp4_i,
    output logic sp4_o,
    output logic sp4_oe,

    input  logic sp5_i,
    output logic sp5_o,
    output logic sp5_oe,

    input  logic sp6_i,
    output logic sp6_o,
    output logic sp6_oe,

    input  logic sp7_i,
    output logic sp7_o,
    output logic sp7_oe,

    input  logic sp8_i,
    output logic sp8_o,
    output logic sp8_oe
);
    // Turn off red NSTATUS LED to indicate programming was successful.
    assign status_no = 1'b1;

    // PMOD1: unused (inputs only, undriven).
    assign pmod1_o [8:1] = '0;
    assign pmod1_oe[8:1] = '0;

    // PMOD2: SuperPET RS-232, Digilent Pmod Interface Type-4 UART pinout so
    // a PmodUSBUART (or any Type-4 USB-serial module) plugs straight in:
    //   header pin 1 = ~CTS in   (module's RTS#)
    //   header pin 2 = TXD out   (module's RXD)
    //   header pin 3 = RXD in    (module's TXD)
    //   header pin 4 = ~RTS out  (module's CTS#)
    // 3.3V LVTTL, idle high. Header pins 7-10 (pmod2[5..8]) unused.
    logic uart_txd, uart_rts_n;
    assign pmod2_o [8:1] = { 4'b0000, uart_rts_n, 1'b0, uart_txd, 1'b0 };
    assign pmod2_oe[8:1] = 8'b0000_1010;   // drive header pins 2 (TXD) and 4 (~RTS)

    // Efinity Interface Designer generates a separate output enable for each bus signal.
    // Create a combined logic signal to control OE for cpu_addr_o[15:0].
    logic cpu_addr_merged_oe;

    assign cpu_addr_oe = {
        cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe,
        cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe,
        cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe,
        cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe
    };

    // Efinity Interface Designer generates a separate output enable for each bus signal.
    // Create a combined logic signal to control OE for cpu_data_o[7:0].
    logic cpu_data_merged_oe;

    assign cpu_data_oe = {
        cpu_data_merged_oe, cpu_data_merged_oe, cpu_data_merged_oe, cpu_data_merged_oe,
        cpu_data_merged_oe, cpu_data_merged_oe, cpu_data_merged_oe, cpu_data_merged_oe
    };

    // For consistency and simplicity, convert active-low signals to active-high signals.
    logic ram_oe_o;
    assign ram_oe_n_o  = !ram_oe_o;

    logic ram_we_o;
    assign ram_we_n_o  = !ram_we_o;

    logic io_oe_o;
    assign io_oe_n_o   = !io_oe_o;

    logic pia1_cs_o;
    assign pia1_cs_n_o = !pia1_cs_o;

    logic pia2_cs_o;
    assign pia2_cs_n_o = !pia2_cs_o;

    logic via_cs_o;
    assign via_cs_n_o  = !via_cs_o;

    logic cpu_we_o, cpu_we_i;
    assign cpu_we_i    = !cpu_we_n_i;
    assign cpu_we_n_o  = !cpu_we_o;

    // RES, IRQ, and NMI are active-low open-drain wire-or signals.  For consistency
    // and simplicity we convert these to active-high outputs and handle OE here.
    logic cpu_reset_i, cpu_reset_o;
    assign cpu_reset_i    = !cpu_reset_n_i;
    assign cpu_reset_n_o  = 0;          // Wire-or only driven when asserted.
    assign cpu_reset_n_oe = cpu_reset_o;

    logic cpu_irq_i, cpu_irq_o;
    assign cpu_irq_i    = !cpu_irq_n_i;
    assign cpu_irq_n_o  = 0;            // Wire-or only driven when asserted.
    assign cpu_irq_n_oe = cpu_irq_o;

    logic cpu_nmi_i, cpu_nmi_o;
    assign cpu_nmi_i    = !cpu_nmi_n_i;
    assign cpu_nmi_n_o  = 0;            // Wire-or only driven when asserted.
    assign cpu_nmi_n_oe = cpu_nmi_o;

    // Configure unused spare pins as inputs.
    logic [8:1] spare_i_unused;

    assign spare_i_unused = {sp8_i, sp7_i, sp6_i, sp5_i, sp4_i, sp3_i, sp2_i, sp1_i};
    assign {sp8_o, sp7_o, sp6_o, sp5_o, sp4_o, sp3_o, sp2_o, sp1_o} = '1;
    assign {sp8_oe, sp7_oe, sp6_oe, sp5_oe, sp4_oe, sp3_oe, sp2_oe, sp1_oe} = '0;

    main main (
        .sys_clock_i(sys_clock_i),

        .cpu_reset_i(cpu_reset_i),
        .cpu_reset_o(cpu_reset_o),
        .cpu_be_o(cpu_be_o),
        .cpu_ready_o(cpu_ready_o),
        .cpu_clock_o(cpu_clock_o),
        .cpu_irq_i(cpu_irq_i),
        .cpu_irq_o(cpu_irq_o),
        .cpu_nmi_i(cpu_nmi_i),
        .cpu_nmi_o(cpu_nmi_o),
        .cpu_sync_i(cpu_sync_i),

        .cpu_addr_i(cpu_addr_i),
        .cpu_addr_o(cpu_addr_o),
        .cpu_addr_oe(cpu_addr_merged_oe),

        .cpu_data_i(cpu_data_i),
        .cpu_data_o(cpu_data_o),
        .cpu_data_oe(cpu_data_merged_oe),

        .cpu_we_i(cpu_we_i),
        .cpu_we_o(cpu_we_o),
        .cpu_we_oe(cpu_we_n_oe),

        .uart_txd_o(uart_txd),
        .uart_rxd_i(pmod2_i[3]),
        .uart_rts_n_o(uart_rts_n),
        // The 6551 gates TX on ~CTS, so a floating pin would mute TX. Weak
        // pull-down on pin 1 (see EconoPET.peri.xml) keeps a 3-wire cable
        // transmitting.
        .uart_cts_n_i(pmod2_i[1]),

        .ram_addr_a16_o(ram_addr_a16_o),
        .ram_addr_a15_o(ram_addr_a15_o),
        .ram_addr_a11_o(ram_addr_a11_o),
        .ram_addr_a10_o(ram_addr_a10_o),
        .ram_oe_o(ram_oe_o),
        .ram_we_o(ram_we_o),

        .io_oe_o(io_oe_o),
        .pia1_cs_o(pia1_cs_o),
        .pia2_cs_o(pia2_cs_o),
        .via_cs_o(via_cs_o),

        // Video
        .config_crt_i(config_crt_i),
        .graphic_i(graphic_i),
        .horiz_drive_o(horiz_drive_o),
        .vert_drive_o(vert_drive_o),
        .jiffy_clock_o(jiffy_clock_o),
        .video_o(video_o),

        // Audio
        .diag_i(diag_i),
        .via_cb2_i(via_cb2_i),
        .audio_o(audio_o),
        .audio_det_i(!audio_det_n_i),

        // Keyboard
        .config_keyboard_i(config_keyboard_i),

        // SPI
        .spi0_cs_ni(spi0_cs_ni),
        .spi0_sck_i(spi0_sck_i),
        .spi0_sd_i(spi0_sd_i),
        .spi0_sd_o(spi0_sd_o),
        .spi1_cs_ni(spi1_cs_ni),
        .spi1_sck_i(spi1_sck_i),
        .spi1_sd_i(spi1_sd_i),
        .spi1_sd_o(spi1_sd_o),
        .spi_stall_o(spi_stall_o)
    );
endmodule
