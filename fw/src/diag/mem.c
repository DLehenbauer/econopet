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
#include "mem.h"

#include "driver.h"
#include "fatal.h"
#include "log/log.h"

// SRAM address range (128KB)
#define SRAM_ADDR_MIN 0x00000
#define SRAM_ADDR_MAX 0x1FFFF

// BRAM address range (4KB character ROM)
// WB_BRAM_BASE = 5'b01101, followed by 15 zeros = 0x68000
#define BRAM_ADDR_MIN 0x68000
#define BRAM_ADDR_MAX 0x68FFF

// Current test range (set by test functions)
static int32_t addr_min;
static int32_t addr_max;

bool check_byte(uint32_t addr, uint8_t actual, uint8_t expected) {
    if (actual != expected) {
        // GCC doesn't recognize the %b format until v12.  (However, the runtime supports it.)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wformat"
        #pragma GCC diagnostic ignored "-Wformat-extra-args"
        log_debug("$%05lx: Expected %08b, but got %08b", addr, expected, actual);
        #pragma GCC diagnostic pop

        #ifdef CONTINUE_ON_ERROR
            return false;
        #else
            panic("");
        #endif
    }
    return true;
}

void check_bit(uint32_t addr, uint8_t actual_byte, uint8_t bit_index, uint8_t expected_bit) {
    uint8_t actual_bit = (actual_byte >> bit_index) & 1;
    
    // #pragma GCC diagnostic push
    // #pragma GCC diagnostic ignored "-Wformat"
    // #pragma GCC diagnostic ignored "-Wformat-extra-args"
    vet(actual_bit == expected_bit, "$%05lx[%d]: Expected %d, but got %d (byte read: %08b)", addr, bit_index, expected_bit, actual_bit, actual_byte);
    // #pragma GCC diagnostic pop
}

uint8_t toggle_bit(uint32_t addr, uint8_t byte, uint8_t bit_index, uint8_t expected_bit) {
    assert(addr_min <= addr && addr <= addr_max);
    assert(bit_index <= 7);

    check_bit(addr, byte, bit_index, expected_bit);
    return byte ^ (1 << bit_index);
}

typedef uint8_t last_read_fn();
typedef void last_write_fn(uint8_t byte);
typedef void march_element_fn(int32_t addr, int8_t bit, last_read_fn* pLastReadFn);

void test_each_bit_ascending(march_element_fn* pFn) {
    spi_read_seek(addr_min);    
    for (int32_t addr = addr_min; addr <= addr_max; addr++) {
        for (int8_t bit = 0; bit < 7; bit++) {
            pFn(addr, bit, spi_read_same);
        }
        pFn(addr, 7, addr != addr_max ? spi_read_next : spi_read_same);
    }
}

void test_each_bit_descending(march_element_fn* pFn) {
    spi_read_seek(addr_max);
    for (int32_t addr = addr_max; addr >= addr_min; addr--) {
        for (int8_t bit = 7; bit >= 1; bit--) {
            pFn(addr, bit, spi_read_same);
        }
        pFn(addr, 0, addr != addr_min ? spi_read_prev : spi_read_same);
    }
}

void r0w1r1(int32_t addr, int8_t bit, last_read_fn* pLastReadFn) {
    uint8_t byte = spi_read_same();
    byte = toggle_bit(addr, byte, bit, /* expected: */ 0);
    spi_write_same(byte);
    spi_read_same();
    byte = pLastReadFn();
    check_bit(addr, byte, bit, /* expected: */ 1);
}

void r0w1(int32_t addr, int8_t bit, last_read_fn* pLastReadFn) {
    uint8_t byte = toggle_bit(addr, spi_read_same(), bit, /* expected: */ 0);
    spi_write_same(byte);
    pLastReadFn(byte);
}

void r1w0(int32_t addr, int8_t bit, last_read_fn* pLastReadFn) {
    uint8_t byte = toggle_bit(addr, spi_read_same(), bit, /* expected: */ 1);
    spi_write_same(byte);
    pLastReadFn(byte);
}

// Uses Extended March C- algorithm
// See: https://booksite.elsevier.com/9780123705976/errata/13~Chapter%2008%20MBIST.pdf
static void march_c_minus(const char* name) {
    log_debug("  %s: $%05lx-$%05lx", name, addr_min, addr_max);

    log_debug("    ⇕(w0): ");
    spi_fill(addr_min, 0, addr_max - addr_min + 1);
    log_debug("OK");

    log_debug("    ⇑(r0,w1,r1): ");
    test_each_bit_ascending(r0w1r1);
    log_debug("OK");

    log_debug("    ⇑(r1,w0): ");
    test_each_bit_ascending(r1w0);
    log_debug("OK");

    log_debug("    ⇓(r0,w1): ");
    test_each_bit_descending(r0w1);
    log_debug("OK");
    
    log_debug("    ⇓(r1,w0): ");
    test_each_bit_descending(r1w0);
    log_debug("OK");

    log_debug("    ⇕(r0): ");
    spi_read_seek(addr_min);
    for (int32_t addr = addr_min; addr <= addr_max; addr++) {
        check_byte(addr, spi_read_next(), 0);
    }
    log_debug("OK");
}

void test_ram() {
    for (uint32_t iteration = 1;; iteration++) {
        log_debug("Extended March C- Test: Iteration #%ld:", iteration);
        addr_min = SRAM_ADDR_MIN;
        addr_max = SRAM_ADDR_MAX;
        march_c_minus("RAM");

        addr_min = BRAM_ADDR_MIN;
        addr_max = BRAM_ADDR_MAX;
        march_c_minus("BRAM");
    }
}
