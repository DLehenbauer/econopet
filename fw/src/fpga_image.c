// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "fpga_image.h"

/// EFP1 header size: magic plus little-endian 32-bit version, length and CRC.
#define FPGA_IMAGE_HEADER_SIZE 16u
/// Supported image-format version, independent of the gateware release.
#define FPGA_IMAGE_VERSION 1u
/// Reflected CRC-32/ISO-HDLC polynomial, matching the image writer's zlib CRC.
#define CRC32_POLYNOMIAL 0xedb88320u
/// Maximum payload bytes copied into RAM for one callback invocation.
#define FPGA_IMAGE_CHUNK_SIZE 256u

/**
 * @brief Decode a little-endian header field without requiring an aligned pointer.
 * @param bytes Pointer to at least four readable bytes.
 * @return The decoded unsigned 32-bit value.
 */
static uint32_t read_u32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/**
 * @brief Validate our custom header and produce a borrowed, bounded payload view.
 * @param image Readable storage of at least capacity bytes.
 * @param capacity Available storage in bytes, including the header.
 * @param result Output view whose data pointer borrows image storage.
 * @return True for supported headers with a nonempty payload that fits capacity.
 *         On failure, result is unchanged.
 * @note This checks metadata only. Payload CRC validation occurs during streaming.
 */
bool fpga_image_read_header(const uint8_t* image, size_t capacity, fpga_image_t* result) {
    // Reject short or unsupported headers before interpreting the payload fields.
    if (capacity < FPGA_IMAGE_HEADER_SIZE || memcmp(image, "EFP1", 4) != 0 ||
        read_u32(image + 4) != FPGA_IMAGE_VERSION) {
        return false;
    }

    // Bound the declared payload to the remaining storage after the header.
    const uint32_t size = read_u32(image + 8);
    if (size == 0 || size > capacity - FPGA_IMAGE_HEADER_SIZE) {
        return false;
    }

    // Publish the view only after validation, without copying or owning the payload.
    *result = (fpga_image_t) {
        .data = image + FPGA_IMAGE_HEADER_SIZE,
        .length = size,
        .crc = read_u32(image + 12),
    };
    return true;
}

/**
 * @brief Deliver the payload in RAM-backed chunks while calculating its CRC-32.
 * @param image Payload view and expected CRC. Its storage must remain readable
 *              throughout the call.
 * @param callback Synchronous consumer returning false to abort delivery. Chunk
 *                 storage is reused and must not be retained after the callback.
 * @param context Opaque caller context forwarded unchanged to each callback.
 * @return True if every callback succeeds and the final CRC matches. Returns
 *         false immediately on callback failure, or after delivery on CRC mismatch.
 * @warning Final CRC validation does not undo data already consumed by callbacks.
 */
bool fpga_image_stream(const fpga_image_t* image, fpga_image_read_callback_t callback, void* context) {
    // Reuse one stack buffer and initialize CRC-32/ISO-HDLC's all-ones accumulator.
    uint8_t buffer[FPGA_IMAGE_CHUNK_SIZE];
    uint32_t crc = UINT32_MAX;

    // Read the payload once, delivering each bounded chunk before advancing.
    for (size_t offset = 0; offset < image->length;) {
        // Limit the final chunk to the remaining bytes without padding the payload.
        const size_t remaining = image->length - offset;
        const size_t count = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

        // Hash the same copied bytes that the callback receives, not a second flash read.
        for (size_t i = 0; i < count; i++) {
            buffer[i] = image->data[offset + i];
            crc ^= buffer[i];

            // Subtracting the low bit from zero selects the polynomial without a branch.
            for (unsigned int bit = 0; bit < 8; bit++) {
                crc = (crc >> 1) ^ ((0u - (crc & 1u)) & CRC32_POLYNOMIAL);
            }
        }

        // Require synchronous acceptance before reusing the buffer for another chunk.
        if (!callback(buffer, count, context)) {
            return false;
        }
        offset += count;
    }

    // Apply the CRC's final XOR only after the complete payload has been delivered.
    return (crc ^ UINT32_MAX) == image->crc;
}
