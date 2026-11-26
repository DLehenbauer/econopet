// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "ieee/diskimage_file.h"

#include "global.h"

/**
 * Create or overwrite a zero-filled raw disk container at path.
 *
 * Streams zeroed chunks through the shared temporary buffer, flushes the
 * underlying FILE handle, and unlinks any incomplete output on error.
 */
bool diskimage_create_file(const char* const path, const diskimage_type_t type) {
    // Reject missing paths or unsupported image types before touching the filesystem.
    const uint32_t total_bytes = diskimage_size(type);
    if (path == NULL || path[0] == '\0' || total_bytes == 0) {
        return false;
    }

    // Open for binary output so callers such as menus or diagnostics can handle errors.
    FILE* const file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }

    // Stream zero-filled chunks through the shared scratch buffer.
    uint8_t* buffer = acquire_temp_buffer();
    memset(buffer, 0, TEMP_BUFFER_SIZE);

    bool ok = true;
    uint32_t remaining = total_bytes;
    while (remaining != 0) {
        const size_t chunk = remaining < TEMP_BUFFER_SIZE ? remaining : TEMP_BUFFER_SIZE;
        if (fwrite(buffer, 1, chunk, file) != chunk) {
            ok = false;
            break;
        }
        remaining -= (uint32_t)chunk;
    }

    release_temp_buffer(&buffer);

    // Ensure buffered clusters reach storage before publishing success.
    if (ok && fflush(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }

    // Clean up incomplete files so failed creations do not leave invalid images behind.
    if (!ok) {
        (void)remove(path);
    }
    return ok;
}
