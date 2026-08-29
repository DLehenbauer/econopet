// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "system_state.h"

void driver_init();

void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest);
void spi_read_seek(uint32_t addr);
uint8_t spi_read_at(uint32_t addr);
uint8_t spi_read_next();
uint8_t spi_read_prev();
uint8_t spi_read_same();

void spi_write(uint32_t addr, const uint8_t* const pSrc, size_t byteLength);

void spi_write_same_block(uint32_t addr, const uint8_t* const pSrc, size_t byteLength);
uint8_t spi_write_at(uint32_t addr, uint8_t data);
uint8_t spi_write_next(uint8_t data);
uint8_t spi_write_prev(uint8_t data);
uint8_t spi_write_same(uint8_t data);
void spi_fill(uint32_t addr, uint8_t byte, size_t byteLength);

void set_cpu(bool ready, bool reset, bool nmi);

// In-fabric CPU select (REG_CPU_SEL). Switching does not reconfigure
// the FPGA.
typedef enum {
    CPU_PHYS_6502 = 0,   // socketed W65C02S (optional; may be depopulated)
    CPU_SOFT_6809 = 1,   // soft MC6809 (SuperPET)
    CPU_SOFT_6502 = 2,   // soft MOS 6502 (virtual PET CPU; the default)
    CPU_AUTO      = 0xFF, // firmware policy (NOT a REG_CPU_SEL value): use the
                          // physical 6502 if detected, else the soft 6502.
} cpu_type_t;

// Caller must hold the CPU halted across the switch. CPU_AUTO is not a
// valid argument.
void set_cpu_type(cpu_type_t cpu);

// True if a W65C02S is socketed. First call probes (clobbers $0400-$0402
// and $FFFC), then caches.
bool physical_cpu_present(void);

void sync_state();

uint16_t bp_hit_addr();
void bp_clear_halt();

void read_pet_model(system_state_t* const system_state);
void write_pet_model(const system_state_t* const system_state);
