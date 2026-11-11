// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "driver.h"

#include "fatal.h"

#define PET_IO_FIRST 0xe810u
#define PET_IO_LAST 0xe84fu

/** Read once without queuing a successor transaction. */
uint8_t spi_read_once(const uint32_t address) {
    spi_read_seek(address);
    return spi_read_pop();
}

/** Translate the supported PIA/VIA registers, excluding the unmapped lower hole. */
static uint32_t pet_io_addr(const uint16_t address) {
    vet(address >= PET_IO_FIRST && address <= PET_IO_LAST, "PET I/O address out of range: $%04X", address);
    return ECONOPET_WB_IO_BASE_ADDR | (address & 0x7f);
}

/** Read one physical register while the CPU is halted. */
uint8_t pet_io_read(const uint16_t address) {
    vet(!(get_cpu() & CPU_READY), "PET I/O access while CPU is ready: $%04X", address);
    return spi_read_once(pet_io_addr(address));
}

/** Write one physical register while the CPU is halted. */
void pet_io_write(const uint16_t address, const uint8_t value) {
    vet(!(get_cpu() & CPU_READY), "PET I/O access while CPU is ready: $%04X", address);
    spi_write_at(pet_io_addr(address), value);
}

/** Write guest READY/RESET/NMI control. */
void set_cpu(const cpu_state_t state) { spi_write_at(ECONOPET_WB_CPU_ADDR, (uint8_t)state); }

/** Read guest control flags. */
cpu_state_t get_cpu(void) { return (cpu_state_t)(spi_read_at(ECONOPET_WB_CPU_ADDR) & ECONOPET_REG_CPU_MASK); }
