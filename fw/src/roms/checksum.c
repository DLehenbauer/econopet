// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "checksum.h"

void checksum_add(const uint8_t* buffer, size_t length, uint8_t* checksum) {
    unsigned int a = *checksum;
    
    for (size_t i = 0; i < length; i++) {
        a += buffer[i];     // Add next byte to accumulator
        a += (a >> 8);      // Add carry from high byte to low byte
        a &= 0xff;          // Keep only low byte
    }

    *checksum = (uint8_t) a;
}

uint8_t checksum_fix(uint8_t current_byte, int actual_sum, int expected_sum) {
    return current_byte + (expected_sum - actual_sum);
}
