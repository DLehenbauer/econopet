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
 
 module top(
    // FPGA
    input  logic         clk_i,             // 64 MHz clock (from PLL)
    output logic         status_no,         // NSTATUS LED (0 = On, 1 = Off)
    
    // SPI1 bus
    input  logic         spi1_cs_ni,        // (CS)  Chip Select (active low)
    input  logic         spi1_sck_i,        // (SCK) Serial Clock
    input  logic         spi1_sd_i,         // (SDI) Serial Data In (rx)
    output logic         spi1_sd_o,         // (SDO) Serial Data Out (tx)

    // Config
    input  logic         config_crt_i,      // (0 = 12", 1 = 9")
    input  logic         config_kbd_i,      // (0 = Business, 1 = Graphics)
    
    // Spare pins
    output logic  [9:0]  spare_o
);
    logic [7:0] spi1_rx;
    logic       spi1_cycle;

    spi spi1(
        .spi_cs_ni(spi1_cs_ni),
        .spi_sck_i(spi1_sck_i),
        .spi_sd_i(spi1_sd_i),
        .spi_sd_o(spi1_sd_o),

        .data_o(spi1_rx),
        .cycle_o(spi1_cycle)
    );

    assign spare_o[7:0] = spi1_rx;
    assign spare_o[8]   = spi1_cycle;
endmodule
