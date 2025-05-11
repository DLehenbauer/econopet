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

#include "global.h"

static uint8_t __buffer[TEMP_BUFFER_SIZE] = { 0 };
static uint8_t* temp_buffer = __buffer;

uint8_t* acquire_temp_buffer() {
    assert(temp_buffer != NULL);

    uint8_t* ptr = temp_buffer;
    temp_buffer = NULL;
    return ptr;
}

void release_temp_buffer(uint8_t** const buffer) {
    assert(temp_buffer == NULL);
    assert(*buffer == __buffer);

    temp_buffer = __buffer;
    *buffer = NULL;
}
