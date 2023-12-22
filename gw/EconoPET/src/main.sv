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
    parameter DATA_WIDTH = 8,
    parameter WB_ADDR_WIDTH = 20,
    parameter CPU_ADDR_WIDTH = 16,
    parameter RAM_ADDR_WIDTH = 17
)(
    // FPGA
    input  logic         clock_i,           // 64 MHz clock (from PLL)
    output logic         status_no,         // NSTATUS LED (0 = On, 1 = Off)

    // CPU
    output logic                       cpu_be_o,

    output logic                       cpu_addr_oe,

    input  logic [DATA_WIDTH-1:0]      cpu_data_i,
    output logic [DATA_WIDTH-1:0]      cpu_data_o,
    output logic                       cpu_data_oe,

    // RAM
    output logic [RAM_ADDR_WIDTH-1:0]  ram_addr_o,
    output logic ram_oe_o,
    output logic ram_we_o,

    // SPI1 bus
    input  logic         spi1_cs_ni,        // (CS)  Chip Select (active low)
    input  logic         spi1_sck_i,        // (SCK) Serial Clock
    input  logic         spi1_sd_i,         // (SDI) Serial Data In (MCU -> FPGA)
    output logic         spi1_sd_o,         // (SDO) Serial Data Out (FPGA -> MCU)
    output logic         spi_stall_o,
    
    // Spare pins
    output logic  [9:0]  spare_o
);
    logic        reset = '0;

    logic [WB_ADDR_WIDTH-1:0] spi1_addr;
    logic [DATA_WIDTH-1:0]    spi1_data_rx;
    logic [DATA_WIDTH-1:0]    spi1_data_tx;
    logic                     spi1_we;
    logic                     spi1_cycle;
    logic                     spi1_strobe;
    logic                     bram_ack;

    spi1_controller spi1(
        .wb_clock_i(clock_i),
        .wb_addr_o(spi1_addr),
        .wb_data_o(spi1_data_rx),
        .wb_data_i(spi1_data_tx),
        .wb_we_o(spi1_we),
        .wb_cycle_o(spi1_cycle),
        .wb_strobe_o(spi1_strobe),
        .wb_ack_i(bram_ack),

        .spi_cs_ni(spi1_cs_ni),
        .spi_sck_i(spi1_sck_i),
        .spi_sd_i(spi1_sd_i),
        .spi_sd_o(spi1_sd_o),
        
        .spi_stall_o(spi_stall_o)
    );

    // bram bram(
    //     .wb_clock_i(clock_i),
    //     .wb_reset_i(reset),
    //     .wb_addr_i(spi1_addr[9:0]),
    //     .wb_data_i(spi1_data_rx),
    //     .wb_data_o(spi1_data_tx),
    //     .wb_we_i(spi1_we),
    //     .wb_cycle_i(spi1_cycle),
    //     .wb_strobe_i(spi1_strobe),
    //     .wb_ack_o(bram_ack)
    // );

    assign cpu_addr_oe = 1'b1;

    ram ram(
        .wb_clock_i(clock_i),
        .wb_reset_i(reset),
        .wb_addr_i(spi1_addr[16:0]),
        .wb_data_i(spi1_data_rx),
        .wb_data_o(spi1_data_tx),
        .wb_we_i(spi1_we),
        .wb_cycle_i(spi1_cycle),
        .wb_strobe_i(spi1_strobe),
        .wb_ack_o(bram_ack),

        .ram_oe_o(ram_oe_o),
        .ram_we_o(ram_we_o),
        .ram_addr_o(ram_addr_o),
        .ram_data_i(cpu_data_i),
        .ram_data_o(cpu_data_o),
        .ram_data_oe(cpu_data_oe)
    );

    assign spare_o[DATA_WIDTH-1:0] = spi1_data_rx;
    assign spare_o[8]              = spi1_cycle;
    assign spare_o[9]              = bram_ack;
endmodule