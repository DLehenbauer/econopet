// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdint.h>

#define TEMP_BUFFER_SIZE 2048

uint8_t* acquire_temp_buffer();
void release_temp_buffer(uint8_t** buffer);
