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
