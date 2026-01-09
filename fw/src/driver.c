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

#include "pch.h"
#include "driver.h"

#include "display/dvi/dvi.h"
#include "fatal.h"
#include "global.h"
#include "hw.h"
#include "usb/keyboard.h"

//                           WMd_AAAA
#define SPI_CMD_READ_AT    0b01000000
#define SPI_CMD_READ_NEXT  0b00100000
#define SPI_CMD_READ_PREV  0b01100000
#define SPI_CMD_READ_SAME  0b00000000
#define SPI_CMD_WRITE_AT   0b11000000
#define SPI_CMD_WRITE_NEXT 0b10100000
#define SPI_CMD_WRITE_PREV 0b11100000
#define SPI_CMD_WRITE_SAME 0b10000000

#define ADDR_KBD  (0b011 << 17)
#define ADDR_CRTC (0b0101 << 16)

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
#define REG_VIDEO_80_COL_MODE   (1 << 0)
#define REG_VIDEO_RAM_MASK_LO   (1 << 1)
#define REG_VIDEO_RAM_MASK_HI   (1 << 2)
#define REG_VIDEO_RAM_MASK_SHIFT 1       // Bit position where the 2-bit RAM mask starts

/**
 * Begins an SPI command transaction with the FPGA.
 * 
 * This function:
 * 1. Waits for SPI_STALL_GP to be deasserted (low), indicating the FPGA is ready
 * 2. Asserts CS (chip select) low to begin the next command
 */
static void cmd_start() {
    // Typically, SPI_STALL_GP should already be low before starting a new command.
    while (gpio_get(SPI_STALL_GP));

    // The PrimeCell SSP deasserts CS after each byte is transmitted.  This conflicts with the
    // FPGA SPI state machine, which resets when CS is deasserted, canceling the previous command
    // and deasserting STALL in response (whether the previous command has completed or not).
    //
    // Consequently, we control the CS signal manually (but use the PrimeCell SSP for the
    // the other SPI signals.)
    gpio_put(FPGA_SPI_CSN_GP, 0);
}

/**
 * Ends an SPI command transaction with the FPGA.
 * 
 * This function:
 * 1. Waits for SPI_STALL_GP to be deasserted (low), ensuring the FPGA has completed processing
 * 2. Deasserts CS (chip select) high to end the transaction.
 */
static void cmd_end() {
    // Deasserting CS implicitly resets the FPGA SPI state machine, causing SPI_STALL_GP to be
    // deasserted early.  Therefore, we must wait until SPI_STALL_GP is low before deasserting CS.
    while (gpio_get(SPI_STALL_GP));

    gpio_put(FPGA_SPI_CSN_GP, 1);
}

/**
 * Queues a read operation at the specified address (pipelined read setup).
 * 
 * This function initiates a READ operation but does NOT return any data. The FPGA begins
 * an asynchronous read at the specified address. To retrieve the result, you must call
 * one of the spi_read_*() or spi_write_*() functions to retrieve the result (while
 * simultaneously queuing the next operation).
 * 
 * PIPELINE PROTOCOL:
 * The SPI protocol uses pipelined reads for efficiency:
 * 1. spi_read_seek(addr) - Queue read of address 'addr' (no data returned yet)
 * 2. spi_read_next()     - Returns data from 'addr', queues read of 'addr+1'
 * 3. spi_read_next()     - Returns data from 'addr+1', queues read of 'addr+2'
 * 
 * This pipelining allows efficient sequential access with only 8 bits TX/RX per byte.
 * 
 * @param addr 20-bit address to read from
 */
void spi_read_seek(uint32_t addr) {
    // Upper 4 bits of the address are encoded in the command byte
    const uint8_t cmd = SPI_CMD_READ_AT | (addr >> 16);
    const uint8_t addr_hi = addr >> 8;
    const uint8_t addr_lo = addr;
    const uint8_t tx[] = { cmd, addr_hi, addr_lo };

    cmd_start();
    spi_write_blocking(FPGA_SPI_INSTANCE, tx, sizeof(tx));
    cmd_end();
}

/**
 * Reads a single byte from the specified address.
 * 
 * This is a convenience function that combines spi_read_seek() and spi_read_next().
 * Due to the pipelined protocol, this requires TWO SPI transactions:
 * 1. Queue the read at the specified address (3 bytes TX)
 * 2. Retrieve the result (1 byte TX/RX)
 * 
 * For sequential reads, use spi_read_seek() once followed by multiple spi_read_next()
 * calls for better efficiency.
 * 
 * @param addr 20-bit address to read from
 * @return The byte value at the specified address
 */
uint8_t spi_read_at(uint32_t addr) {
    spi_read_seek(addr);
    return spi_read_next();
}

/**
 * Retrieves the result of the previous read and queues a read of the next sequential address.
 * 
 * WHY THIS RETURNS THE PREVIOUS READ:
 * The SPI protocol uses pipelined reads where commands and results are temporally separated:
 * 
 * Timeline:
 * T1: Send READ_NEXT command → FPGA begins async read of current address
 * T2: [FPGA processing - SPI_STALL_GP asserted]
 * T3: Next READ_* command → FPGA returns result from T1, starts new read
 * 
 * The bidirectional nature of SPI means that when you send the READ_NEXT command byte,
 * you simultaneously RECEIVE the data byte from the FPGA. However, the FPGA can't return
 * data for the CURRENT command yet (it hasn't been processed), so it returns data from
 * the PREVIOUS command.
 * 
 * This pipelining enables optimal 1:1 efficiency (8 bits TX + 8 bits RX = 1 byte read).
 * 
 * USAGE PATTERN:
 *   spi_read_seek(0x1000);    // Queue read of 0x1000
 *   uint8_t b0 = spi_read_next(); // Returns data[0x1000], queues read of 0x1001
 *   uint8_t b1 = spi_read_next(); // Returns data[0x1001], queues read of 0x1002
 *   uint8_t b2 = spi_read_next(); // Returns data[0x1002], queues read of 0x1003
 * 
 * @return Data byte from the previously queued read operation
 */
uint8_t spi_read_next() {
    const uint8_t tx[1] = { SPI_CMD_READ_NEXT };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();
    
    return rx[0];
}

/**
 * Retrieves the result of the previous read and queues a read of the previous sequential address.
 * 
 * Like spi_read_next(), this returns data from the PREVIOUS read operation due to the
 * pipelined protocol. The difference is that this decrements the address pointer instead
 * of incrementing it.
 * 
 * WHY THIS RETURNS THE PREVIOUS READ:
 * See detailed explanation in spi_read_next() documentation. The same pipelined protocol
 * applies - you cannot get results for a command that hasn't been processed yet.
 * 
 * USAGE PATTERN:
 *   spi_read_seek(0x1000);    // Queue read of 0x1000
 *   uint8_t b0 = spi_read_prev(); // Returns data[0x1000], queues read of 0x0FFF
 *   uint8_t b1 = spi_read_prev(); // Returns data[0x0FFF], queues read of 0x0FFE
 * 
 * This enables efficient backward sequential access with 1:1 efficiency (8 bits TX/RX per byte).
 * 
 * @return Data byte from the previously queued read operation
 */
uint8_t spi_read_prev() {
    const uint8_t tx[1] = { SPI_CMD_READ_PREV };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();
    
    return rx[0];
}

/**
 * Retrieves the result of the previous read and queues another read of the same address.
 * 
 * Like spi_read_next(), this returns data from the PREVIOUS read operation due to the
 * pipelined protocol. The address pointer remains unchanged.
 * 
 * WHY THIS RETURNS THE PREVIOUS READ:
 * See detailed explanation in spi_read_next() documentation. The same pipelined protocol
 * applies - you cannot get results for a command that hasn't been processed yet.
 * 
 * USAGE PATTERN:
 *   spi_read_seek(0x1000);    // Queue read of 0x1000
 *   uint8_t b0 = spi_read_same(); // Returns data[0x1000], queues read of 0x1000
 *   uint8_t b1 = spi_read_same(); // Returns data[0x1000], queues read of 0x1000
 * 
 * This is useful for re-reading volatile memory locations or hardware registers that may
 * change between reads, with 1:1 efficiency (8 bits TX/RX per byte).
 * 
 * @return Data byte from the previously queued read operation
 */
uint8_t spi_read_same() {
    const uint8_t tx[1] = { SPI_CMD_READ_SAME };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();
    
    return rx[0];
}

/**
 * Reads a contiguous block of memory from the FPGA.
 * 
 * This function efficiently reads sequential bytes using the pipelined protocol:
 * 1. spi_read_seek(addr) - Queue initial read (3 bytes TX)
 * 2. Loop: spi_read_next() - Each iteration transfers only 1 byte TX/RX
 * 
 * EFFICIENCY:
 * - Initial setup: 3 bytes TX (address + command)
 * - Per byte: 1 byte TX + 1 byte RX (1:1 ratio - optimal for SPI)
 * - Total for N bytes: (3 + N) bytes TX, N bytes RX
 * 
 * The pipelined protocol makes sequential reads very efficient compared to random access,
 * which would require 4 bytes TX per byte read (3-byte address + 1-byte command each time).
 * 
 * @param addr Starting address to read from
 * @param byteLength Number of bytes to read
 * @param pDest Destination buffer (must be at least byteLength bytes)
 */
void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest) {
    spi_read_seek(addr);

    while (byteLength--) {
        *pDest++ = spi_read_next();
    }
}

/**
 * Writes a single byte to the specified address AND returns the byte from any previously queued read.
 * 
 * Unlike reads, writes are NOT pipelined - the data is written immediately.
 * The FPGA asserts SPI_STALL_GP while processing the write, and cmd_end()
 * waits for the write to complete.
 * 
 * PROTOCOL:
 * TX: [cmd | addr[19:16], addr[15:8], addr[7:0], data] (4 bytes total)
 * RX: [previous_read_result, garbage, garbage, garbage] (4 bytes total)
 * - cmd byte encodes upper 4 address bits
 * - Followed by 2 address bytes and 1 data byte
 * 
 * HOW THE RETURN VALUE WORKS:
 * Like spi_read_write_next(), this exploits the pipelined protocol. The bidirectional
 * nature of SPI means we receive 4 bytes while transmitting 4 bytes. The first RX byte
 * (rx[0]) contains the result of any previously queued read operation.
 * 
 * USAGE PATTERN:
 *   spi_read_seek(0x1000);           // Queue read of 0x1000
 *   uint8_t b = spi_write_at(0x2000, 0xAA);  // Write 0xAA to 0x2000, returns data[0x1000]
 * 
 * EFFICIENCY:
 * 4:1 ratio (4 bytes TX per byte written), but you get a free read from the pipeline.
 * 
 * For sequential writes, use spi_write_at() once followed by spi_write_next()
 * to achieve 2:1 efficiency.
 * 
 * @param addr 20-bit address to write to
 * @param data Byte value to write
 * @return The byte value from the previously queued read operation (or garbage if no read was queued)
 */
uint8_t spi_write_at(uint32_t addr, uint8_t data) {
    const uint8_t cmd = SPI_CMD_WRITE_AT | addr >> 16;
    const uint8_t addr_hi = addr >> 8;
    const uint8_t addr_lo = addr;
    const uint8_t tx[] = { cmd, addr_hi, addr_lo, data };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();
    
    return rx[0];
}

/**
 * Writes a byte to the next sequential address and returns the previously queued read value.
 * 
 * This command writes to (previous_address + 1). The FPGA maintains an internal
 * address pointer that is automatically incremented.
 * 
 * PROTOCOL:
 * TX: [SPI_CMD_WRITE_NEXT, data] (2 bytes)
 * RX: [previously_queued_read, garbage] (2 bytes)
 * 
 * The returned value (rx[0]) is the result of any previously queued READ operation.
 * This does NOT represent a read-before-write of the target address being written.
 * 
 * EFFICIENCY:
 * Sequential writes achieve 2:1 efficiency (2 bytes TX per 1 byte written).
 * 
 * USAGE PATTERN:
 *   spi_write_at(0x1000, 0xAA);    // Write 0xAA to 0x1000 (4 bytes TX)
 *   spi_write_next(0xBB);          // Write 0xBB to 0x1001 (2 bytes TX/RX)
 *   spi_write_next(0xCC);          // Write 0xCC to 0x1002 (2 bytes TX/RX)
 * 
 * @param data Byte value to write
 * @return Previously queued read value (or garbage if no read was queued)
 */
uint8_t spi_write_next(uint8_t data) {
    const uint8_t tx [] = { SPI_CMD_WRITE_NEXT, data };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();

    return rx[0];
}

/**
 * Writes a byte to the previous sequential address and returns the previously queued read value.
 * 
 * This command writes to (previous_address - 1). The FPGA maintains an internal
 * address pointer that is automatically decremented.
 * 
 * PROTOCOL:
 * TX: [SPI_CMD_WRITE_PREV, data] (2 bytes)
 * RX: [previously_queued_read, garbage] (2 bytes)
 * 
 * The returned value (rx[0]) is the result of any previously queued READ operation.
 * This does NOT represent a read-before-write of the target address being written.
 * 
 * EFFICIENCY: 2:1 (same as spi_write_next)
 * 
 * This enables efficient backward sequential writes with the same 2:1 efficiency
 * as forward writes.
 * 
 * @param data Byte value to write
 * @return Previously queued read value (or garbage if no read was queued)
 */
uint8_t spi_write_prev(uint8_t data) {
    const uint8_t tx [] = { SPI_CMD_WRITE_PREV, data };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();

    return rx[0];
}

/**
 * Writes a byte to the same address as the previous operation and returns the previously queued read value.
 * 
 * This command writes to the same address without modifying the FPGA's internal
 * address pointer.
 * 
 * PROTOCOL:
 * TX: [SPI_CMD_WRITE_SAME, data] (2 bytes)
 * RX: [previously_queued_read, garbage] (2 bytes)
 * 
 * The returned value (rx[0]) is the result of any previously queued READ operation.
 * This does NOT represent a read-before-write of the target address being written.
 * 
 * EFFICIENCY: 2:1 (same as spi_write_next)
 * 
 * This is useful for writing to volatile memory locations or hardware registers
 * multiple times, or for implementing simple RMW (read-modify-write) patterns.
 * 
 * @param data Byte value to write
 * @return Previously queued read value (or garbage if no read was queued)
 */
uint8_t spi_write_same(uint8_t data) {
    const uint8_t tx [] = { SPI_CMD_WRITE_SAME, data };
    uint8_t rx[sizeof(tx)];

    cmd_start();
    spi_write_read_blocking(FPGA_SPI_INSTANCE, tx, rx, sizeof(tx));
    cmd_end();

    return rx[0];
}

/**
 * Writes a contiguous block of memory to the FPGA.
 * 
 * This function efficiently writes sequential bytes using the sequential write commands:
 * 1. spi_write_at(addr, first_byte) - Initial write with full address (4 bytes TX)
 * 2. Loop: spi_write_next(byte) - Each iteration transfers 2 bytes TX
 * 
 * EFFICIENCY:
 * - Initial write: 4 bytes TX (command + 3-byte address + data)
 * - Per additional byte: 2 bytes TX (command + data)
 * - Total for N bytes: (2 + 2*N) bytes TX
 * 
 * This is half as efficient as sequential reads (which achieve 1:1 after setup)
 * because writes must transmit both command AND data in the TX direction only,
 * while reads benefit from bidirectional transfer (TX command while RX data).
 * 
 * @param addr Starting address to write to
 * @param pSrc Source buffer containing bytes to write
 * @param byteLength Number of bytes to write
 */
void spi_write(uint32_t addr, const uint8_t* const pSrc, size_t byteLength) {
    const uint8_t* p = pSrc;
    
    if (byteLength--) {
        spi_write_at(addr, *p++);

        while (byteLength--) {
            spi_write_next(*p++);
        }
    }
}

/**
 * Writes a block of memory to the FPGA while simultaneously reading previously queued values.
 * 
 * This advanced function leverages the pipelined protocol to perform a block write
 * while capturing previously queued read results from the pipeline.
 * 
 * PROTOCOL SEQUENCE:
 * 1. spi_write_at(addr, write_data[0])  - Write first byte, returns previously queued read
 * 2. Loop for remaining bytes:
 *    spi_write_next(write_data[i])      - Write next byte, returns previously queued read
 * 3. spi_read_next()                     - Retrieve final queued read from pipeline
 * 
 * IMPORTANT - THIS DOES NOT READ THE ADDRESSES BEING WRITTEN:
 * The returned values come from whatever reads were previously queued in the pipeline,
 * NOT from the addresses being written to. To read the values at the target addresses
 * before overwriting them, you must explicitly queue reads of those addresses first.
 * 
 * EXAMPLE - Reading values before overwriting:
 * ```
 * spi_read_seek(0x1000);               // Queue read of 0x1000
 * uint8_t orig[3];
 * spi_write_read(0x1000, new_data, orig, 3);  // Overwrites 0x1000-0x1002, captures original values
 * ```
 * 
 * The final spi_read_next() is needed because the pipeline always has one pending result.
 * 
 * EFFICIENCY:
 * - Initial: 4 bytes TX (write_at with address)
 * - Per byte: 2 bytes TX/RX (write_next + read previous)
 * - Final: 1 byte TX/RX (read last value)
 * - Total: (4 + 2*N + 1) bytes TX, N bytes RX
 * 
 * USE CASES:
 * - Memory swapping operations
 * - Atomic read-modify-write patterns
 * - Debugging (capture before overwriting)
 * 
 * @param addr Starting address for the write/read operation
 * @param pWriteSrc Source buffer containing bytes to write
 * @param pReadDest Destination buffer for original values (before overwrite)
 * @param byteLength Number of bytes to write/read
 */
void spi_write_read(uint32_t addr, const uint8_t* const pWriteSrc, uint8_t* pReadDest, size_t byteLength) {
    const uint8_t* p = pWriteSrc;
    
    if (byteLength--) {
        *pReadDest++ = spi_write_at(addr, *p++);

        while (byteLength--) {
            *pReadDest++ = spi_write_next(*p++);
        }

        *pReadDest++ = spi_read_next();
    }
}

/**
 * Fills a range of FPGA memory with a repeated byte value (similar to memset).
 * 
 * This function uses a temporary buffer to batch writes for better efficiency.
 * Instead of writing one byte at a time, it fills a buffer with the repeated
 * value and writes entire chunks using spi_write().
 * 
 * STRATEGY:
 * 1. Acquire temporary buffer (size TEMP_BUFFER_SIZE)
 * 2. Fill buffer with the repeated byte value
 * 3. Write buffer in chunks using sequential write commands
 * 4. Repeat until all bytes are written
 * 
 * EFFICIENCY:
 * By using spi_write() for chunks, we achieve:
 * - 4 bytes TX for first byte per chunk (write_at)
 * - 2 bytes TX per additional byte in chunk (write_next)
 * 
 * This is much more efficient than individual spi_write_at() calls which
 * would require 4 bytes TX per byte (no amortization of address overhead).
 * 
 * @param addr Starting address to fill
 * @param byte Value to fill memory with
 * @param byteLength Number of bytes to fill
 */
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

/**
 * Controls the PET CPU state via the CPU control register.
 * 
 * This function writes to REG_CPU, which controls three CPU signals:
 * - READY: CPU clock enable (false = halted, true = running)
 * - RESET: CPU reset signal (true = held in reset)
 * - NMI: Non-maskable interrupt (true = NMI asserted)
 *
 * These signals allow the RP2040 to control the 6502 CPU (via the FPGA),
 * enabling operations like halting execution to modify memory, triggering resets,
 * or injecting NMI interrupts.
 * 
 * @param ready If true, CPU clock is enabled (CPU runs); if false, CPU is halted
 * @param reset If true, CPU is held in reset state
 * @param nmi If true, NMI (non-maskable interrupt) is asserted
 */
void set_cpu(bool ready, bool reset, bool nmi) {
    uint8_t state = 0;
    if (ready) { state |= REG_CPU_READY; }
    if (reset) { state |= REG_CPU_RESET; }
    if (nmi)   { state |= REG_CPU_NMI; }
    spi_write_at(REG_CPU, state);
}

/**
 * Reads the PET hardware configuration from the FPGA status register.
 * 
 * The FPGA reads physical DIP switches on the hardware to determine which
 * PET model variant is being emulated. This function reads REG_STATUS and
 * decodes the DIP switch settings into the system_state structure.
 * 
 * Note: DIP switches are active low (0 = ON, 1 = OFF).
 * 
 * STATUS REGISTER BITS:
 * - REG_STATUS_CRT (bit 1): Video type
 *   0 = 12" CRTC display (20kHz, CRTC)
 *   1 = 9" fixed display (15kHz, non-CRTC)
 * 
 * - REG_STATUS_KEYBOARD (bit 2): Keyboard type  
 *   0 = Business keyboard (no graphics characters)
 *   1 = Graphics keyboard (with graphics characters)
 * 
 * @param system_state Pointer to system state structure to populate with hardware config
 */
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

/**
 * Writes the PET display configuration to the FPGA video control register.
 * 
 * This function configures the video subsystem in the FPGA based on the
 * current system state, including display column mode and video RAM size.
 * 
 * Video control register bits:
 * 
 * - REG_VIDEO_80_COL_MODE (bit 0): Column mode
 *   0 = 40 column mode
 *   1 = 80 column mode
 * 
 * - REG_VIDEO_RAM_MASK (bits 2:1): Video RAM size mask
 *   00 = 1KB at $8000 (40 column monochrome)
 *   01 = 2KB at $8000 (80 column monochrome)
 *   10 = 1KB at $8000 + 1KB at $8800 (40 column color)
 *   11 = 4KB at $8000 (80 column color)
 * 
 * @param system_state Pointer to system state containing display configuration
 */
void write_pet_model(const system_state_t* const system_state) {
    // Ensure derived fields are consistent.
    vet(system_state->video_ram_bytes == (size_t)(system_state->video_ram_mask + 1) * 1024u,
        "system_state.video_ram_bytes (%zu) inconsistent with video_ram_mask (%u)",
        system_state->video_ram_bytes, system_state->video_ram_mask);
    uint8_t state = 0;

    if (system_state->pet_display_columns == pet_display_columns_80) {
        state |= REG_VIDEO_80_COL_MODE;
    }

    state |= (system_state->video_ram_mask << REG_VIDEO_RAM_MASK_SHIFT);

    spi_write_at(REG_VIDEO, state);
}

/**
 * Synchronizes keyboard and video state between the RP2040 and FPGA.
 * 
 * This function is called periodically to maintain consistent state between
 * the RP2040 (which handles USB keyboard input and video output) and the
 * FPGA (which generates native PET video and injects USB keyboard input).
 * 
 * Operations performed:
 * 
 * 1. Write USB keyboard matrix to FPGA (ADDR_KBD)
 *    - RP2040 scans USB keyboard and updates usb_key_matrix[]
 *    - FPGA reads this to inject key presses when the CPU reads $E812.
 * 
 * 2. Read PET keyboard matrix from FPGA (ADDR_KBD)
 *    - Reads the current PET keyboard state as seen by the CPU.
 *    - This allows RP2040 to know which keys are pressed on the PET keyboard.
 * 
 * 3. Read CRTC registers from FPGA (ADDR_CRTC)
 *    - CRTC (cathode ray tube controller) registers control video timing
 *    - Used by the RP2040 to emulate CRTC when generating DVI/TMDS video
 * 
 * 4. Read graphics mode flag from status register (upper/lower case)
 *    - Used by the RP2040 to renderer characters when generating DVI/TMDS video
 */
void sync_state() {
    // Write USB keyboard matrix state to FPGA
    spi_write(ADDR_KBD, usb_key_matrix, KEY_COL_COUNT);

    // Read PET keyboard matrix from FPGA
    spi_read(ADDR_KBD, KEY_COL_COUNT, pet_key_matrix);

    // Read CRTC registers from FPGA
    spi_read(ADDR_CRTC, CRTC_REG_COUNT, pet_crtc_registers);

    // Read graphics mode flag from status register
    uint8_t status = spi_read_at(REG_STATUS);
    system_state.video_graphics = (status & REG_STATUS_GRAPHICS) != 0;
}
