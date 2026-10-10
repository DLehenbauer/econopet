// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

// Full EconoPET FPGA boundary for C++ system tests. It preserves production
// top/main address decoding, soft CPU selection, and SPI-to-Wishbone behavior.
module system (
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

    input  logic [15:0] cpu_addr_i,
    output logic [15:0] cpu_addr_o,
    output logic [15:0] cpu_addr_oe,

    input  logic [7:0] cpu_data_i,
    output logic [7:0] cpu_data_o,
    output logic [7:0] cpu_data_oe,

    input  logic cpu_we_n_i,
    output logic cpu_we_n_o,
    output logic cpu_we_n_oe,

    input  logic cpu_irq_n_i,
    input  logic io_irq_ni,     // Combined open-drain IRQ from the external PIA/VIA models
    output logic cpu_irq_n_o,
    output logic cpu_irq_n_oe,

    input  logic cpu_nmi_n_i,
    output logic cpu_nmi_n_o,
    output logic cpu_nmi_n_oe,

    input  logic cpu_sync_i,    // Asserted when the physical CPU fetches an opcode.

    // RAM
    output logic ram_addr_a10_o,
    output logic ram_addr_a11_o,
    output logic ram_addr_a15_o,
    output logic ram_addr_a16_o,
    output logic ram_oe_n_o,
    output logic ram_we_n_o,

    // IO
    output logic io_oe_n_o,
    output logic pia1_cs_n_o,   // (CS2B)
    output logic pia2_cs_n_o,   // (CS2B)
    output logic via_cs_n_o,    // (CS2B)
    output logic pia1_clock_o,  // Isolated keyboard-scanning PHI2

    // SPI buses
    input  logic spi_cs_ni,     // SPI0 chip select (active low)
    input  logic spi_sck_i,     // SPI0 serial clock
    input  logic spi_sdo_i,     // SPI0 data from MCU to FPGA
    output logic spi_sdi_o,     // SPI0 data from FPGA to MCU

    input  logic spi1_cs_ni,    // SPI1 chip select (active low)
    input  logic spi1_sck_i,    // SPI1 serial clock
    input  logic spi1_sd_i,     // SPI1 data from MCU to FPGA
    input  logic spi1_sdo_i,    // Shared FPGA SDO / MCU SDI / SD-card DAT0
    output logic spi1_sdo_o,
    output logic spi1_sdo_oe,

    output logic spi_stall_o,   // Flow control for SPI (0 = Ready, 1 = Busy)

    // Future bidirectional interfaces
    input  logic i2c0_scl_i,
    output logic i2c0_scl_o,
    output logic i2c0_scl_oe,
    input  logic i2c0_sda_i,
    output logic i2c0_sda_o,
    output logic i2c0_sda_oe,

    input  logic i2c1_scl_i,
    output logic i2c1_scl_o,
    output logic i2c1_scl_oe,
    input  logic i2c1_sda_i,
    output logic i2c1_sda_o,
    output logic i2c1_sda_oe,

    input  logic mcu_cec_i,
    output logic mcu_cec_o,
    output logic mcu_cec_oe,

    // Config from DIP switch
    input logic config_crt_i,       // Display type (0 = 12"/CRTC/20kHz, 1 = 9"/non-CRTC/15kHz)
    input logic config_hz_i,        // Refresh rate (0 = 60 Hz, 1 = 50 Hz)
    input logic config_keyboard_i,  // Keyboard type (0 = Business, 1 = Graphics)

    // Video
    input  logic graphic_i,         // VIA CA2 -> Character ROM A10 (0 = graphics, 1 = text)
    output logic horiz_drive_o,     // Horizontal drive for native PET video
    output logic vert_drive_o,      // Vertical drive for native PET video
    output logic jiffy_clock_o,     // Triggers IRQ on falling edge (VIA CB1)
    output logic video_o,

    // Audio
    input  logic diag_i,
    input  logic via_cb2_i,
    output logic audio_l_o,
    output logic audio_r_o,
    input  logic audio_det_i,       // Detects 3.5mm jack insertion (1 = inserted)

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

    input  logic sp6_i,
    output logic sp6_o,
    output logic sp6_oe,

    input  logic sp7_i,
    output logic sp7_o,
    output logic sp7_oe,

    input  logic sp8_i,
    output logic sp8_o,
    output logic sp8_oe,

    // Resolved open-drain reset observed by production top and both soft cores.
    output logic cpu_reset_active_o,
    output logic [1:0] cpu_selection_o
);
    // The board's pulled-up reset net combines the FPGA and external drivers.
    wire cpu_reset_n = cpu_reset_n_i && (!cpu_reset_n_oe || cpu_reset_n_o);
    assign cpu_reset_active_o = !cpu_reset_n;
    assign cpu_selection_o = top.main.cpu_sel;

    top top (
        .sys_clock_i,
        .status_no,
        .cpu_reset_n_i(cpu_reset_n),
        .cpu_reset_n_o,
        .cpu_reset_n_oe,
        .cpu_be_o,
        .cpu_clock_o,
        .cpu_ready_o,
        .cpu_addr_i,
        .cpu_addr_o,
        .cpu_addr_oe,
        .cpu_data_i,
        .cpu_data_o,
        .cpu_data_oe,
        .cpu_we_n_i,
        .cpu_we_n_o,
        .cpu_we_n_oe,
        .cpu_irq_n_i(cpu_irq_n_i && io_irq_ni),
        .cpu_irq_n_o,
        .cpu_irq_n_oe,
        .cpu_nmi_n_i,
        .cpu_nmi_n_o,
        .cpu_nmi_n_oe,
        .cpu_sync_i,
        .ram_addr_a10_o,
        .ram_addr_a11_o,
        .ram_addr_a15_o,
        .ram_addr_a16_o,
        .ram_oe_n_o,
        .ram_we_n_o,
        .io_oe_n_o,
        .pia1_cs_n_o,
        .pia2_cs_n_o,
        .via_cs_n_o,
        .pia1_clock_o,
        .spi0_cs_ni(spi_cs_ni),
        .spi0_sck_i(spi_sck_i),
        .spi0_sd_i(spi_sdo_i),
        .spi0_sd_o(spi_sdi_o),
        .spi1_cs_ni,
        .spi1_sck_i,
        .spi1_sd_i,
        .spi1_sdo_i,
        .spi1_sdo_o,
        .spi1_sdo_oe,
        .spi_stall_o,
        .i2c0_scl_i,
        .i2c0_scl_o,
        .i2c0_scl_oe,
        .i2c0_sda_i,
        .i2c0_sda_o,
        .i2c0_sda_oe,
        .i2c1_scl_i,
        .i2c1_scl_o,
        .i2c1_scl_oe,
        .i2c1_sda_i,
        .i2c1_sda_o,
        .i2c1_sda_oe,
        .mcu_cec_i,
        .mcu_cec_o,
        .mcu_cec_oe,
        .config_crt_i,
        .config_hz_i,
        .config_keyboard_i,
        .graphic_i,
        .horiz_drive_o,
        .vert_drive_o,
        .jiffy_clock_o,
        .video_o,
        .diag_i,
        .via_cb2_i,
        .audio_l_o,
        .audio_r_o,
        .audio_det_n_i(!audio_det_i),
        .pmod1_i,
        .pmod1_o,
        .pmod1_oe,
        .pmod2_i,
        .pmod2_o,
        .pmod2_oe,
        .sp1_i,
        .sp1_o,
        .sp1_oe,
        .sp2_i,
        .sp2_o,
        .sp2_oe,
        .sp3_i,
        .sp3_o,
        .sp3_oe,
        .sp6_i,
        .sp6_o,
        .sp6_oe,
        .sp7_i,
        .sp7_o,
        .sp7_oe,
        .sp8_i,
        .sp8_o,
        .sp8_oe
    );

endmodule
