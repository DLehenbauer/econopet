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

module main #(
    parameter integer unsigned WB_ADDR_WIDTH = 20,
    parameter integer unsigned RAM_ADDR_WIDTH = 17,
    parameter integer unsigned CPU_ADDR_WIDTH = 16,
    parameter integer unsigned DATA_WIDTH = 8
) (
    // FPGA
    input  logic clock_i,   // 64 MHz clock (from PLL)

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

    output logic io_oe_o,
    output logic pia1_cs_o,
    output logic pia2_cs_o,
    output logic via_cs_o,

    // Video
    output logic v_sync_o,

    // SPI1 bus
    input  logic spi1_cs_ni,  // (CS)  Chip Select (active low)
    input  logic spi1_sck_i,  // (SCK) Serial Clock
    input  logic spi1_sd_i,   // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi1_sd_o,   // (SDO) Serial Data Out (FPGA -> MCU)
    output logic spi_stall_o
);
    // Currently, there is no wishbone reset.
    wire reset       = '0;

    logic [WB_ADDR_WIDTH-1:0] spi1_addr;
    logic [   DATA_WIDTH-1:0] spi1_data_rx;
    logic [   DATA_WIDTH-1:0] spi1_data_tx;
    logic                     spi1_we;
    logic                     spi1_cycle;
    logic                     spi1_strobe;
    logic                     spi1_stall;
    logic                     spi1_ack;

    spi1_controller spi1 (
        .wb_clock_i(clock_i),
        .wb_addr_o(spi1_addr),
        .wb_data_o(spi1_data_rx),
        .wb_data_i(spi1_data_tx),
        .wb_we_o(spi1_we),
        .wb_cycle_o(spi1_cycle),
        .wb_strobe_o(spi1_strobe),
        .wb_stall_i(spi1_stall),
        .wb_ack_i(spi1_ack),

        .spi_cs_ni(spi1_cs_ni),
        .spi_sck_i(spi1_sck_i),
        .spi_sd_i (spi1_sd_i),
        .spi_sd_o (spi1_sd_o),

        .spi_stall_o(spi_stall_o)
    );

    system system (
        // Wishbone
        .wb_clock_i(clock_i),
        .wb_reset_i(reset),
        .wb_addr_i(spi1_addr),
        .wb_data_i(spi1_data_rx),
        .wb_data_o(spi1_data_tx),
        .wb_we_i(spi1_we),
        .wb_cycle_i(spi1_cycle),
        .wb_strobe_i(spi1_strobe),
        .wb_stall_o(spi1_stall),
        .wb_ack_o(spi1_ack),

        // CPU
        .cpu_reset_i(cpu_reset_i),
        .cpu_reset_o(cpu_reset_o),
        .cpu_be_o(cpu_be_o),
        .cpu_ready_o(cpu_ready_o),
        .cpu_clock_o(cpu_clock_o),
        .cpu_irq_i(cpu_irq_i),
        .cpu_irq_o(cpu_irq_o),
        .cpu_nmi_i(cpu_nmi_i),
        .cpu_nmi_o(cpu_nmi_o),
        .cpu_addr_i(cpu_addr_i),
        .cpu_addr_o(cpu_addr_o),
        .cpu_addr_oe(cpu_addr_oe),
        .cpu_data_i(cpu_data_i),
        .cpu_data_o(cpu_data_o),
        .cpu_data_oe(cpu_data_oe),        
        .cpu_we_i(cpu_we_i),
        .cpu_we_o(cpu_we_o),
        .cpu_we_oe(cpu_we_oe),
        
        // RAM
        .ram_addr_a10_o(ram_addr_a10_o),
        .ram_addr_a11_o(ram_addr_a11_o),
        .ram_addr_a15_o(ram_addr_a15_o),
        .ram_addr_a16_o(ram_addr_a16_o),
        .ram_oe_o(ram_oe_o),
        .ram_we_o(ram_we_o),

        // IO
        .io_oe_o(io_oe_o),
        .pia1_cs_o(pia1_cs_o),
        .pia2_cs_o(pia2_cs_o),
        .via_cs_o(via_cs_o)
    );

    vsync vsync (
        .cpu_clock_i(cpu_clock_o),
        .v_sync_o(v_sync_o)
    );
endmodule
