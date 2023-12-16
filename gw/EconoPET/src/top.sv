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
 
 module top #(
    parameter DATA_WIDTH = 8,
    parameter WB_ADDR_WIDTH = 20,
    parameter CPU_ADDR_WIDTH = 16,
    parameter RAM_ADDR_WIDTH = 17
) (
    // CPU
    input  logic [CPU_ADDR_WIDTH-1:0]  cpu_addr_i,
    output logic [CPU_ADDR_WIDTH-1:0]  cpu_addr_o,
    output logic [CPU_ADDR_WIDTH-1:0]  cpu_addr_oe,

    input  logic [DATA_WIDTH-1:0]      cpu_data_i,
    output logic [DATA_WIDTH-1:0]      cpu_data_o,
    output logic [DATA_WIDTH-1:0]      cpu_data_oe,

    // RAM
    output logic ram_addr_a10_o,
    output logic ram_addr_a11_o,
    output logic ram_addr_a15_o,
    output logic ram_addr_a16_o,
    output logic ram_oe_n_o,
    output logic ram_we_n_o,

    // FPGA
    input  logic         clock_i,           // 64 MHz clock (from PLL)
    output logic         status_no,         // NSTATUS LED (0 = On, 1 = Off)
    
    // SPI1 bus
    input  logic         spi1_cs_ni,        // (CS)  Chip Select (active low)
    input  logic         spi1_sck_i,        // (SCK) Serial Clock
    input  logic         spi1_sd_i,         // (SDI) Serial Data In (MCU -> FPGA)
    output logic         spi1_sd_o,         // (SDO) Serial Data Out (FPGA -> MCU)
    output logic         spi_stall_o,

    // Config
    input  logic         config_crt_i,      // (0 = 12", 1 = 9")
    input  logic         config_kbd_i,      // (0 = Business, 1 = Graphics)
    
    // Spare pins
    output logic  [9:0]  spare_o
);
    main main(
        .clock_i(clock_i),
        .status_no(status_no),
        .spi1_cs_ni(spi1_cs_ni),
        .spi1_sck_i(spi1_sck_i),
        .spi1_sd_i(spi1_sd_i),
        .spi1_sd_o(spi1_sd_o),
        .spi_stall_o(spi_stall_o),
        .spare_o(spare_o)
    );
endmodule
