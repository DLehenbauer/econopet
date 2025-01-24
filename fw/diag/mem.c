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

//#define CONTINUE_ON_ERROR

const int32_t addr_min = 0x00000;
const int32_t addr_max = 0x1FFFF;

bool check_byte(uint32_t addr, uint8_t actual, uint8_t expected) {
    if (actual != expected) {
        // GCC doesn't recognize the %b format until v12.  (However, the runtime supports it.)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wformat"
        #pragma GCC diagnostic ignored "-Wformat-extra-args"
        printf("$%05lx: Expected %08b, but got %08b\n", addr, expected, actual);
        #pragma GCC diagnostic pop

        #ifdef CONTINUE_ON_ERROR
            return false;
        #else
            panic("");
        #endif
    }
    return true;
}

void fix_byte(uint32_t addr, uint8_t expected) {
    int i = 0;

    for (; i < 8; i++) {
        spi_write_at(addr, expected);
        if (check_byte(addr, spi_read_at(addr), expected)) {
            return;
        }
    }

    printf("$%05lx: Correction failed after %d attempts.\n", addr, i);
    panic("");
}

uint8_t check_bit(uint32_t addr, uint8_t actual_byte, uint8_t bit, uint8_t expected_bit) {
    uint8_t actual_bit = (actual_byte >> bit) & 1;
    
    if (actual_bit != expected_bit) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wformat"
        #pragma GCC diagnostic ignored "-Wformat-extra-args"
        printf("$%05lx[%d]: Expected %d, but got %d (actual byte read %08b)\n", addr, bit, expected_bit, actual_bit, actual_byte);
        #pragma GCC diagnostic pop

        sleep_ms(1);
        uint8_t byte2 = spi_read_at(addr);

        if (byte2 != actual_byte) {
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wformat"
            #pragma GCC diagnostic ignored "-Wformat-extra-args"
            printf("Inconsistent read result (attempt 1: %08b, attempt 2: %08b)\n", actual_byte, byte2);
            #pragma GCC diagnostic pop
        }

        #ifdef CONTINUE_ON_ERROR
            // Correct the error before continuing:
            actual_byte = actual_byte ^ (1 << bit);
            fix_byte(addr, actual_byte);
        #else
            panic("");
        #endif
    }

    return actual_byte;
}

uint8_t toggle_bit(uint32_t addr, uint8_t bit, uint8_t expected_bit) {
    assert(addr_min <= addr && addr <= addr_max);
    assert(bit <= 7);

    uint8_t byte = check_bit(addr, spi_read_at(addr), bit, expected_bit);
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

void r0w1r1(int32_t addr, int8_t bit) {
    toggle_bit(addr, bit, /* expected: */ 0);
    check_bit(addr, spi_read_at(addr), bit, /* expected: */ 1);
}

void r0w1(int32_t addr, int8_t bit) {
    toggle_bit(addr, bit, /* expected: */ 0);
}

void r1w0(int32_t addr, int8_t bit) {
    toggle_bit(addr, bit, /* expected: */ 1);
}

// Uses Extended March C- algorithm
// See: https://booksite.elsevier.com/9780123705976/errata/13~Chapter%2008%20MBIST.pdf
void test_ram() {
    for (uint32_t iteration = 1;; iteration++) {
        printf("\nRAM Test (Extended March C-): $%05lx-$%05lx -- Iteration #%ld:\n", addr_min, addr_max, iteration);

        printf("⇕(w0): ");
        spi_write_at(addr_min, 0);
        for (int32_t addr = addr_min + 1; addr <= addr_max; addr++) {
            spi_write_next(0);
        }
        puts("OK");

        printf("⇑(r0,w1,r1): ");
        test_each_bit_ascending(r0w1r1);
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
        spi_read_seek(addr_min);
        for (int32_t addr = addr_min; addr <= addr_max; addr++) {
            check_byte(addr, spi_read_next(), 0);
        }
        puts("OK");
    }
}
