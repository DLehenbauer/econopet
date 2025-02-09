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

#pragma once

#define SPI0_CSN_GP 5
#define SPI0_SCK_GP 6
#define SPI0_SDO_GP 7
#define SPI0_SDI_GP 4

#define SPI1_CSN_GP 13
#define SPI1_SCK_GP 14
#define SPI1_SDO_GP 11
#define SPI1_SDI_GP 12

// When the MCU asserts CS (signaling the beginning of a new command), the FPGA asserts
// STALL until the full command has been received and processed.
#define SPI_STALL_GP 10

// SPI speeds faster than 24 MHz require overclocking 'peri_clk' or using PIO.
// (See: https://github.com/Bodmer/TFT_eSPI/discussions/2432)
#define FPGA_SPI_MHZ 24
#define SD_SPI_MHZ 24

// SPI0 is used to configure the FPGA on POR and then used for communication with
// the FPGA.  Efinix requires SPI mode 3 for configuration.  After POR, SPI mode 0
// is used for communication.
//
// SCK, SDO, and SDI are standard SPI bus signals driven by the PrimeCell SSP.
// The PrimeCell SSP deasserts/reasserts CSN between bytes, which resets the SPI
// state machine on the FPGA (the state machine uses CSN for framing the command).
// Therefore, CSN is driven manually by software as a GPIO.
#define FPGA_SPI_INSTANCE spi0
#define FPGA_SPI_CSN_GP SPI0_CSN_GP
#define FPGA_SPI_SCK_GP SPI0_SCK_GP
#define FPGA_SPI_SDO_GP SPI0_SDO_GP
#define FPGA_SPI_SDI_GP SPI0_SDI_GP

// SPI1 is used to communicate with the SD card.  The SPI1 bus is also connected to
// the FPGA and could be used as a second communication channel in the future.  There
// are separate CSN signals for the SD card and FPGA.
#define SD_SPI_INSTANCE spi1
#define SD_CLK_GP SPI1_SCK_GP
#define SD_CMD_GP SPI1_SDO_GP
#define SD_DAT_GP SPI1_SDI_GP
#define SD_CSN_GP 9
#define SD_DETECT 8

// Pulsing CRESET initiates FPGA configuration.  After the CRESET signal is de-asserted,
// the MCU uploads the FPGA binary via SPI0.
#define FPGA_CRESET_GP 26

// PWM output used to generate the PLL input for FPGA.
#define FPGA_CLK_GP 15

// Menu button (active low / requires enabling pull-up)
#define MENU_BTN_GP 27
