// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "rom4040.h"

#define CONTROLLER4040_IMAGE_SIZE 174848u
#define CONTROLLER4040_TRACKS 35u
#define CONTROLLER4040_DRIVES 2u

// Exact sector I/O, supplied by stdio on the RP2040 or memory in probes.
typedef bool (*controller4040_io_fn)(void* context, uint32_t offset,
    uint8_t sector[ROM4040_SECTOR_SIZE], bool write);

typedef struct {
    controller4040_io_fn io;
    void* context;
    bool writable;
    uint8_t id[2];
} controller4040_disk_t;

typedef struct {
    rom4040_t* machine;
    controller4040_disk_t disk[CONTROLLER4040_DRIVES];
    bool unsupported_code;
} controller4040_t;

// Return the D64 byte offset, or false for an invalid native track/sector.
bool controller4040_offset(unsigned track, unsigned sector, uint32_t* offset);

// Execute stock controller jobs, not arbitrary 6504 machine code.
uint8_t controller4040_job(void* context, uint8_t command,
    uint8_t header[8], uint8_t sector[ROM4040_SECTOR_SIZE]);
