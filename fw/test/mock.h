// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware_contract.h"
#include "test_support.h"

#define MOCK_RAM_SIZE (1u << ECONOPET_CPU_ADDR_WIDTH)
#define MOCK_IEEE_RX_CAPACITY 32
#define MOCK_IEEE_TX_CAPACITY 1024
#define MOCK_IEEE_TXS_CAPACITY 32

extern uint8_t mock_ram[MOCK_RAM_SIZE];

// Resets RAM, register FIFOs, CPU flags, and breakpoint observations.
void mock_reset(void);
// Sets the address returned by the mocked breakpoint register.
void mock_breakpoint_set_hit_addr(uint16_t addr);
// Reports whether the breakpoint halt was cleared since reset.
bool mock_breakpoint_halt_was_cleared(void);

// Queues a receive byte with its IEEE command/data tag.
void mock_ieee_enqueue_rx(bool atn, uint8_t byte);
// Returns the number of captured file-channel bytes.
size_t mock_ieee_data_count(void);
// Returns a captured file-channel byte, asserting the index is valid.
uint8_t mock_ieee_data_byte(size_t index);
// Returns a captured file-channel byte's EOI marker.
bool mock_ieee_data_eoi(size_t index);
// Discards captured file-channel bytes without changing other FIFOs.
void mock_ieee_clear_data(void);
// Returns the number of captured command-channel bytes.
size_t mock_ieee_status_count(void);
// Returns a captured command-channel byte, asserting the index is valid.
uint8_t mock_ieee_status_byte(size_t index);
// Returns a captured command-channel byte's EOI marker.
bool mock_ieee_status_eoi(size_t index);
// Discards captured command-channel bytes without changing other FIFOs.
void mock_ieee_clear_status(void);
