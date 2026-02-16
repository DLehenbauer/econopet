// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
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
