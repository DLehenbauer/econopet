// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t* data;
    size_t length;
    uint32_t crc;
} fpga_image_t;

// Called for each chunk read from flash. Return false to stop streaming.
typedef bool (*fpga_image_read_callback_t)(const uint8_t* data, size_t length, void* context);

// Validate the EFP1 header and return a bounded payload view. Output is unchanged on failure.
bool fpga_image_read_header(const uint8_t* image, size_t capacity, fpga_image_t* result);

// Stream the payload once from flash, checking CRC-32/ISO-HDLC as chunks reach the callback.
// Returns false if the callback fails or the final CRC differs from the header.
bool fpga_image_stream(const fpga_image_t* image, fpga_image_read_callback_t callback, void* context);
