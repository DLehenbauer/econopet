// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "m6502.h"

#define ROM4040_ROM_SIZE 0x3000u
#define ROM4040_SHARED_SIZE 0x1000u
#define ROM4040_RIOT_RAM_SIZE 0x100u
#define ROM4040_JOB_COUNT 15u
#define ROM4040_SECTOR_SIZE 256u

// All signals are physical levels (true means released), including data.
typedef struct {
    bool atn, dav, eoi, nrfd, ndac;
    uint8_t dio;
} rom4040_bus_t;

// Complete a controller request with native controller status (1 is success).
// Header and sector point into shared RAM, not temporary copies.
typedef uint8_t (*rom4040_job_fn)(void* context, uint8_t command,
    uint8_t header[8], uint8_t sector[ROM4040_SECTOR_SIZE]);

typedef enum {
    ROM4040_FAULT_NONE,
    ROM4040_FAULT_CONTROLLER_CODE,
} rom4040_fault_t;

typedef struct {
    uint16_t prescale, divider;
    uint8_t value;
    bool expired, pending, enabled;
    uint32_t underflow;
} rom4040_timer_t;

typedef struct {
    m6502_t cpu;
    uint64_t pins;
    const uint8_t* rom;
    rom4040_job_fn job;
    void* context;
    uint8_t shared[ROM4040_SHARED_SIZE];
    uint8_t riot_ram[ROM4040_RIOT_RAM_SIZE];
    uint8_t ports[2][4];
    rom4040_bus_t input;
    bool edge_enabled[2], edge_positive[2], edge_pending[2], edge_level[2];
    rom4040_timer_t timers[2];
    bool bootstrap, running;
    rom4040_fault_t fault;
    uint32_t cycles, jobs;
} rom4040_t;

// Cold-start the main CPU with unmodified D000-FFFF DOS 2.0 revision-2 ROM.
void rom4040_init(rom4040_t* machine, const uint8_t rom[ROM4040_ROM_SIZE],
    rom4040_job_fn job, void* context);

// Set the virtual controller's line levels and latch RIOT PA7 transitions.
void rom4040_input(rom4040_t* machine, rom4040_bus_t input);

// Read the drive's open-collector contributions, before composing with input.
rom4040_bus_t rom4040_output(const rom4040_t* machine);

// Execute exactly one main-CPU cycle, including memory and controller jobs.
void rom4040_tick(rom4040_t* machine);

// Execute a bounded cooperative slice, stopping on unsupported hardware access.
void rom4040_run(rom4040_t* machine, uint32_t cycles);
