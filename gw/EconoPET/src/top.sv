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
    input  logic clock_i,       // 64 MHz clock (from PLL)
    output logic status_no,     // Red NSTATUS LED (0 = On, 1 = Off)

    // CPU
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

    // SPI1 bus
    input  logic spi1_cs_ni,    // (CS)  Chip Select (active low)
    input  logic spi1_sck_i,    // (SCK) Serial Clock
    input  logic spi1_sd_i,     // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi1_sd_o,     // (SDO) Serial Data Out (FPGA -> MCU)
    output logic spi_stall_o,   // Flow control for SPI1 (0 = Ready, 1 = Busy)

    // Config
    input logic config_crt_i,   // (0 = 12", 1 = 9")
    input logic config_kbd_i,   // (0 = Business, 1 = Graphics)

    // Video
    output logic v_sync_o,

    // Audio
    output logic audio_o,

    // PMOD
    input  logic [8:1] pmod1_i,
    output logic [8:1] pmod1_o,
    output logic [8:1] pmod1_oe,

    input  logic [8:1] pmod2_i,
    output logic [8:1] pmod2_o,
    output logic [8:1] pmod2_oe,

    // Spare pins
    output logic [9:0] spare_o
);
    // Turn off red NSTATUS LED to indicate programming was successful.
    assign status_no = 1'b1;

    // Disable audio output for now.
    assign audio_o = '0;

    // Test PMOD ports by having PMOD1 output whatever PMOD2 inputs.
    assign pmod1_o[8:1] = pmod2_i[8:1];
    assign pmod1_oe[8:1] = '1;
    assign pmod2_oe[8:1] = '0;

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
    logic cpu_irq_i, cpu_irq_o;
    assign cpu_irq_i    = !cpu_irq_n_i;
    assign cpu_irq_n_o  = !cpu_irq_o;
    assign cpu_irq_n_oe = cpu_irq_o;    // Only drive open-drain wire-or when asserted.

    main main (
        .clock_i(clock_i),

        .cpu_be_o(cpu_be_o),
        .cpu_ready_o(cpu_ready_o),
        .cpu_clock_o(cpu_clock_o),

        .cpu_addr_i(cpu_addr_i),
        .cpu_addr_o(cpu_addr_o),
        .cpu_addr_oe(cpu_addr_merged_oe),

        .cpu_data_i(cpu_data_i),
        .cpu_data_o(cpu_data_o),
        .cpu_data_oe(cpu_data_merged_oe),

        .cpu_we_i(cpu_we_i),
        .cpu_we_o(cpu_we_o),
        .cpu_we_oe(cpu_we_n_oe),

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

        .v_sync_o(v_sync_o),

        .spi1_cs_ni(spi1_cs_ni),
        .spi1_sck_i(spi1_sck_i),
        .spi1_sd_i(spi1_sd_i),
        .spi1_sd_o(spi1_sd_o),
        .spi_stall_o(spi_stall_o),
        .spare_o(spare_o)
    );
endmodule
