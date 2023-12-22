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

#include "../driver.h"
#include "mem.h"

const int32_t addr_min = 0x00000;
const int32_t addr_max = 0x1FFFF;

void check_byte(uint32_t addr, uint8_t actual, uint8_t expected) {
    if (actual != expected) {
        printf("$%x: Expected %d, but got %d\n", addr, expected, actual);
        panic("");
    }
}

uint8_t toggle_bit(uint32_t addr, uint8_t bit, uint8_t expected) {
    assert(addr_min <= addr && addr <= addr_max);
    assert(0 <= bit && bit <= 7);

    spi_read_at(addr);
    uint8_t byte = spi_read_next();
    uint8_t actual = (byte >> bit) & 1;
    
    if (actual != expected) {
        printf("$%x[%d]: Expected %d, but got %d (actual byte read %08b)\n", addr, bit, expected, actual, byte);
        panic("");
    }

    byte = byte ^ (1 << bit);
    spi_write_at(addr, byte);
    return byte;
}

typedef void march_element_fn(int32_t addr, int8_t bit);

void test_each_bit_ascending(march_element_fn* pFn) {
    for (int32_t addr = addr_min; addr <= addr_max; addr++) {
        for (int8_t bit = 0; bit < 8; bit++) {
            pFn(addr, bit);
        }
    }
}

void test_each_bit_descending(march_element_fn* pFn) {
    for (int32_t addr = addr_max; addr >= addr_min; addr--) {
        for (int8_t bit = 7; bit >= 0; bit--) {
            pFn(addr, bit);
        }
    }
}

void r0w1(int32_t addr, int8_t bit) {
    toggle_bit(addr, bit, /* expected: */ 0);
}

void r1w0(int32_t addr, int8_t bit) {
    toggle_bit(addr, bit, /* expected: */ 1);
}

void test_ram() {
    for (uint32_t iteration = 1;; iteration++) {
        printf("\nRAM Test (March C-): $%05x-$%05x -- Iteration #%d:\n", addr_min, addr_max, iteration);

        printf("⇕(w0): ");
        spi_write_at(addr_min, 0);
        for (int32_t addr = addr_min + 1; addr <= addr_max; addr++) {
            spi_write_next(0);
        }
        puts("OK");

        printf("⇑(r0,w1): ");
        test_each_bit_ascending(r0w1);
        puts("OK");

        printf("⇑(r1,w0): ");
        test_each_bit_ascending(r1w0);
        puts("OK");

        printf("⇓(r0,w1): ");
        test_each_bit_descending(r0w1);
        puts("OK");
        
        printf("⇓(r1,w0): ");
        test_each_bit_descending(r1w0);
        puts("OK");

        printf("⇕(r0): ");
        spi_read_at(addr_min);
        for (int32_t addr = addr_min; addr <= addr_max; addr++) {
            check_byte(addr, spi_read_next(), 0);
        }
        puts("OK");
    }
}
