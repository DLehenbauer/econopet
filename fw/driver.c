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

#include "driver.h"
#include "hw.h"

//                           WS__AAAA
#define SPI_CMD_READ_AT    0b01000000
#define SPI_CMD_READ_NEXT  0b00000000
#define SPI_CMD_WRITE_AT   0b11000000
#define SPI_CMD_WRITE_NEXT 0b10000000

void cmd_start() {
    while (gpio_get(SPI_STALL_GP));
    gpio_put(SPI_CS_GP, 0);
}

void cmd_end() {
    while (gpio_get(SPI_STALL_GP));
    gpio_put(SPI_CS_GP, 1);
}

uint8_t spi_read_at(uint32_t addr) {
    const uint8_t cmd = SPI_CMD_READ_AT | (addr >> 16);
    const uint8_t addr_hi = addr >> 8;
    const uint8_t addr_lo = addr;
    const uint8_t tx[] = { cmd, addr_hi, addr_lo };

    cmd_start();
    spi_write_blocking(SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

uint8_t spi_read_next() {
    const uint8_t tx[1] = { SPI_CMD_READ_NEXT };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();
    
    return rx[0];
}

void spi_read(uint32_t addr, uint32_t byteLength, uint8_t* pDest) {
    spi_read_at(addr);

    while (byteLength--) {
        *pDest++ = spi_read_next();
    }
}

void spi_write_at(uint32_t addr, uint8_t data) {
    const uint8_t cmd = SPI_CMD_WRITE_AT | addr >> 16;
    const uint8_t addr_hi = addr >> 8;
    const uint8_t addr_lo = addr;
    const uint8_t tx[] = { cmd, addr_hi, addr_lo, data };

    cmd_start();
    spi_write_blocking(SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

void spi_write_next(uint8_t data) {
    const uint8_t tx [] = { SPI_CMD_WRITE_NEXT, data };

    cmd_start();
    spi_write_blocking(SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

void spi_write(uint32_t addr, uint32_t byteLength, const uint8_t const* pSrc) {
    const uint8_t* p = pSrc;
    
    if (byteLength--) {
        spi_write_at(addr, *p++);

        while (byteLength--) {
            spi_write_next(*p++);
        }
    }
}
