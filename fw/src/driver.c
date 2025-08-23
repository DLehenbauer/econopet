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
#include "global.h"

//                           WMd_AAAA
#define SPI_CMD_READ_AT    0b01000000
#define SPI_CMD_READ_NEXT  0b00100000
#define SPI_CMD_READ_PREV  0b01100000
#define SPI_CMD_READ_SAME  0b00000000
#define SPI_CMD_WRITE_AT   0b11000000
#define SPI_CMD_WRITE_NEXT 0b10100000
#define SPI_CMD_WRITE_PREV 0b11100000
#define SPI_CMD_WRITE_SAME 0b10000000

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

static void cmd_start() {
    while (gpio_get(SPI_STALL_GP));

    // The PrimeCell SSP deasserts CS after each byte is transmitted.  This conflicts with the
    // FPGA SPI state machine, which relies on CS for framing the command.
    //
    // As a workaround, we control the CS signal manually (but use the PrimeCell SSP for the
    // the other SPI signals.)
    gpio_put(FPGA_SPI_CSN_GP, 0);
}

static void cmd_end() {
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

uint8_t spi_read_prev() {
    const uint8_t tx[1] = { SPI_CMD_READ_PREV };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();
    
    return rx[0];
}

uint8_t spi_read_same() {
    const uint8_t tx[1] = { SPI_CMD_READ_SAME };
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

void spi_write_prev(uint8_t data) {
    const uint8_t tx [] = { SPI_CMD_WRITE_PREV, data };

    cmd_start();
    spi_write_blocking(FPGA_SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

void spi_write_same(uint8_t data) {
    const uint8_t tx [] = { SPI_CMD_WRITE_SAME, data };

    cmd_start();
    spi_write_blocking(FPGA_SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

uint8_t spi_read_write_next(const uint8_t data) {
    const uint8_t tx [] = { SPI_CMD_WRITE_NEXT, data };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();

    return rx[sizeof(tx) - 1];
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

void spi_write_read(uint32_t addr, const uint8_t* const pWriteSrc, uint8_t* pReadDest, size_t byteLength) {
    const uint8_t* p = pWriteSrc;
    
    if (byteLength--) {
        spi_write_at(addr, *p++);

        while (byteLength--) {
            *pReadDest++ = spi_read_write_next(*p++);
        }

        *pReadDest++ = spi_read_next();
    }
}

// Fills a range of memory with a byte value.
void spi_fill(uint32_t addr, uint8_t byte, size_t byteLength) {
    uint8_t* temp_buffer = acquire_temp_buffer();
    
    size_t chunk_len = MIN(TEMP_BUFFER_SIZE, byteLength);
    memset(temp_buffer, byte, chunk_len);

    int32_t remaining = byteLength;
    while (remaining > 0) {
        spi_write(addr, temp_buffer, chunk_len);
        addr += chunk_len;
        remaining -= chunk_len;
        chunk_len = MIN(chunk_len, remaining);
    }

    release_temp_buffer(&temp_buffer);
}

void set_cpu(bool ready, bool reset, bool nmi) {
    uint8_t state = 0;
    if (ready) { state |= REG_CPU_READY; }
    if (reset) { state |= REG_CPU_RESET; }
    if (nmi)   { state |= REG_CPU_NMI; }
    spi_write_at(REG_CPU, state);
}

void read_pet_model(system_state_t* const system_state) {
    uint8_t status = spi_read_at(REG_STATUS);

    // Map DIP switch position to model flags. (Note that DIP switch is active low.)

    // PET video type (0 = 12"/CRTC/20kHz, 1 = 9"/non-CRTC/15kHz)
    system_state->pet_video_type = (status & REG_STATUS_CRT) == 0
        ? pet_video_type_crtc
        : pet_video_type_fixed;

    // PET keyboard type (0 = Business, 1 = Graphics)
    system_state->pet_keyboard_model = (status & REG_STATUS_KEYBOARD) == 0
        ? pet_keyboard_model_business
        : pet_keyboard_model_graphics;
}

void write_pet_model(const system_state_t* const system_state) {
    uint8_t state = 0;

    if (system_state->pet_display_columns == pet_display_columns_80) {
        state |= REG_VIDEO_80_COL_MODE;
    }

    spi_write_at(REG_VIDEO, state);
}

void sync_state() {
    spi_write(ADDR_KBD, usb_key_matrix, KEY_COL_COUNT);
    spi_read(ADDR_KBD, KEY_COL_COUNT, pet_key_matrix);

    uint8_t status = spi_read_at(REG_STATUS);
    video_graphics = (status & REG_STATUS_GRAPHICS) != 0;
}
