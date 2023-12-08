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

#pragma region // SPI

// SPI speeds faster than 24 MHz require overclocking 'peri_clk' or using PIO for SPI.
// (See: https://github.com/Bodmer/TFT_eSPI/discussions/2432)
#define SPI_MHZ 24

#define SPI_INSTANCE spi1

// SCK, SDO, and SDI are standard SPI bus signals driven by the PrimeCell SSP
#define SPI_SCK_GP 14
#define SPI_SDO_GP 11
#define SPI_SDI_GP 12

// The CS output is controlled via software to avoid de-asserting CS after each byte
// is transmitted, which would asynchronously reset the FPGA's SPI state machine.
#define SPI_CS_GP 13

// When the MCU asserts CS (signalling the beginning of a new command), the FPGA asserts
// STALL until the full command has been received and processed.
#define SPI_STALL_GP 10

#pragma endregion

#pragma region // FPGA

// Pulsing CRESET initiates FPGA configuration.  After the CRESET signal is de-asserted,
// the MCU uploads the FPGA binary via SPI0.
#define FPGA_CRESET_GP 26

// PWM output used to generate the PLL input for FPGA.
#define FPGA_CLK_GP 15

#pragma endregion
