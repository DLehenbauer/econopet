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

#define SPI_INSTANCE spi1
#define SPI_SCK_GP 14
#define SPI_SDO_GP 11
#define SPI_SDI_GP 12
#define SPI_CS_GP 13
#define SPI_STALL_GP 10
#define SPI_MHZ 24

#define FPGA_CRESET_GP 26
#define FPGA_CLK_GP 15
