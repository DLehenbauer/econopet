// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

#include "ieee/diskimage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create or overwrite a zero-filled raw disk container at path.
 *
 * Returns false on invalid arguments or I/O failure, removing any partially
 * written file when a write or flush fails.
 */
bool diskimage_create_file(const char* path, diskimage_type_t type);

#ifdef __cplusplus
}
#endif
