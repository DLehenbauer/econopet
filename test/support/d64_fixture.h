// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ieee/diskimage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fixture allocation reserves the whole directory track, without DOS interleave.
#define D64_FIXTURE_FILES 144u
#define D64_FIXTURE_DATA_SECTORS 664u

typedef struct {
    uint8_t track;
    uint8_t sector;
} d64_fixture_sector_t;

// Construction state is separate from storage, so copying both creates an owner.
typedef struct {
    bool used[DISKIMAGE_D64_SIZE / DISKIMAGE_SECTOR_SIZE];
    unsigned int files;
    bool initialized;
} d64_fixture_layout_t;

// Checked geometry and filename encoding. NULL means success, otherwise the
// returned static diagnostic describes the error. Outputs are unchanged on error.
const char* d64_fixture_offset(unsigned int track, unsigned int sector, size_t* offset);
const char* d64_fixture_name(const char* name, uint8_t encoded[16]);

// Format a 35-track image with a BAM and empty directory. Size must be exact.
const char* d64_fixture_empty(d64_fixture_layout_t* layout, uint8_t* image, size_t size);

// Add a closed PRG, preserving payload bytes (including any caller-supplied load
// address). An empty payload owns one empty sector. NULL/0 chain selects automatic
// allocation. An explicit chain must have exactly max(1, ceil(length/254)) free
// data sectors. All checks precede mutation of the image and construction state.
// Initialize state with d64_fixture_empty first. Payload must not overlap image.
const char* d64_fixture_prg(d64_fixture_layout_t* layout, uint8_t* image, size_t size,
    const char* name, const uint8_t* payload, size_t length,
    const d64_fixture_sector_t* chain, size_t chain_length);

// The same construction contract for ordinary SEQ, PRG, and USR chains.
// Use the corresponding DISKIMAGE_FTYPE_* constant for type.
// REL side sectors are deliberately outside this builder's scope.
const char* d64_fixture_file(d64_fixture_layout_t* layout, uint8_t* image, size_t size,
    unsigned int type, const char* name, const uint8_t* payload, size_t length,
    const d64_fixture_sector_t* chain, size_t chain_length);

#ifdef __cplusplus
}
#endif
