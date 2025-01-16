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
#include "usb/keyboard.h"
#include "video/video.h"

//                           WS__AAAA
#define SPI_CMD_READ_AT    0b01000000
#define SPI_CMD_READ_NEXT  0b00000000
#define SPI_CMD_WRITE_AT   0b11000000
#define SPI_CMD_WRITE_NEXT 0b10000000

#define ADDR_KBD (0b011 << 17)

// Register File
#define ADDR_REG    (0b010 << 17)
#define REG_STATUS  (ADDR_REG | 0x00000)
#define REG_CPU     (ADDR_REG | 0x00001)
#define REG_VIDEO   (ADDR_REG | 0x00002)

// Status Register
#define REG_STATUS_GRAPHICS   (1 << 0)
#define REG_STATUS_CRT        (1 << 1)
#define REG_STATUS_KEYBOARD   (1 << 2)

// CPU Control Register
#define REG_CPU_READY (1 << 0)
#define REG_CPU_RESET (1 << 1)
#define REG_CPU_NMI   (1 << 2)

// Video Control Register
#define REG_VIDEO_80_COL_MODE (1 << 0)

void cmd_start() {
    while (gpio_get(SPI_STALL_GP));

    // The PrimeCell SSP deasserts CS after each byte is transmitted.  This conflicts with the
    // FPGA SPI state machine, which relies on CS for framing the command.
    //
    // As a workaround, we control the CS signal manually (but use the PrimeCell SSP for the
    // the other SPI signals.)
    gpio_put(FPGA_SPI_CSN_GP, 0);
}

void cmd_end() {
    while (gpio_get(SPI_STALL_GP));

    // The PrimeCell SSP deasserts CS after each byte is transmitted.  This conflicts with the
    // FPGA SPI state machine, which relies on CS for framing the command.
    //
    // As a workaround, we control the CS signal manually (but use the PrimeCell SSP for the
    // the other SPI signals.)
    gpio_put(FPGA_SPI_CSN_GP, 1);
}

void spi_read_seek(uint32_t addr) {
    const uint8_t cmd = SPI_CMD_READ_AT | (addr >> 16);
    const uint8_t addr_hi = addr >> 8;
    const uint8_t addr_lo = addr;
    const uint8_t tx[] = { cmd, addr_hi, addr_lo };

    cmd_start();
    spi_write_blocking(FPGA_SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

uint8_t spi_read_at(uint32_t addr) {
    spi_read_seek(addr);
    return spi_read_next();
}

uint8_t spi_read_next() {
    const uint8_t tx[1] = { SPI_CMD_READ_NEXT };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();
    
    return rx[0];
}

void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest) {
    spi_read_seek(addr);

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
    spi_write_blocking(FPGA_SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

void spi_write_next(uint8_t data) {
    const uint8_t tx [] = { SPI_CMD_WRITE_NEXT, data };

    cmd_start();
    spi_write_blocking(FPGA_SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

void spi_write(uint32_t addr, const uint8_t* const pSrc, size_t byteLength) {
    const uint8_t* p = pSrc;
    
    if (byteLength--) {
        spi_write_at(addr, *p++);

        while (byteLength--) {
            spi_write_next(*p++);
        }
    }
}

void set_cpu(bool ready, bool reset, bool nmi) {
    uint8_t state = 0;
    if (ready) { state |= REG_CPU_READY; }
    if (reset) { state |= REG_CPU_RESET; }
    if (nmi)   { state |= REG_CPU_NMI; }
    spi_write_at(REG_CPU, state);
}

void set_video(bool col80) {
    uint8_t state = 0;
    if (col80) { state |= REG_VIDEO_80_COL_MODE; }
    spi_write_at(REG_VIDEO, state);
}

void get_model(bool* crtc, bool* business) {
    uint8_t status = spi_read_at(REG_STATUS);

    // DIP switch is active low
    *crtc = (status & REG_STATUS_CRT) == 0;             // (0 = 12" / CRTC / 20kHz, 1 = 9" / NoCRTC / 15kHz)
    *business = (status & REG_STATUS_KEYBOARD) == 0;    // (0 = Business Keyboard, 1 = Graphics Keyboard)
}

void sync_state() {
    spi_write(ADDR_KBD, key_matrix, KEY_COL_COUNT);
    uint8_t status = spi_read_at(REG_STATUS);
    video_graphics = (status & REG_STATUS_GRAPHICS) != 0;
}
