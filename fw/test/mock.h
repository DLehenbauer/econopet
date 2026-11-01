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

// Resets RAM, register FIFOs, CPU flags, breakpoint observations, and input.
void mock_reset(void);
// Supplies menu input through a test reader, or restores EOF when NULL.
void mock_input_set_reader(int (*reader)(void));
// Sets the address returned by the mocked breakpoint register.
void mock_breakpoint_set_hit_addr(uint16_t addr);
// Reports whether the breakpoint halt was cleared since reset.
bool mock_breakpoint_halt_was_cleared(void);

// Queues a receive byte with its IEEE command/data tag.
void mock_ieee_enqueue_rx(bool atn, uint8_t byte);
// Queues a receive byte with explicit ATN and EOI metadata.
void mock_ieee_enqueue_rx_eoi(bool atn, uint8_t byte, bool eoi);
// Queues an ordered ATN-release event without inventing a received byte.
void mock_ieee_enqueue_atn_end(void);
// Queues the first data DAV-release checkpoint for the specified listener units.
void mock_ieee_enqueue_listen_end(uint8_t units);
// Queue ordered source progress using the hardware owner unit/channel layout.
void mock_ieee_enqueue_talk_event(uint8_t owner, uint8_t flags);
// Returns the number of captured file-channel bytes.
size_t mock_ieee_data_count(void);
// Returns a captured file-channel byte, asserting the index is valid.
uint8_t mock_ieee_data_byte(size_t index);
// Returns a captured file-channel byte's EOI marker.
bool mock_ieee_data_eoi(size_t index);
// Models NRFD-triggered advancement through a prefix of captured data.
void mock_ieee_consume_data(size_t count);
// Models NRFD-triggered advancement through all captured file-channel bytes.
void mock_ieee_clear_data(void);
// Returns the number of captured command-channel bytes.
size_t mock_ieee_status_count(void);
// Returns a captured command-channel byte, asserting the index is valid.
uint8_t mock_ieee_status_byte(size_t index);
// Returns a captured command-channel byte's EOI marker.
bool mock_ieee_status_eoi(size_t index);
// Models NRFD-triggered advancement through a prefix of captured status.
void mock_ieee_consume_status(size_t count);
// Discards status transport storage without fetching any response bytes.
void mock_ieee_discard_status(void);
// Accepts a repeated status secondary after this many TXS writes, flushing storage.
void mock_ieee_repeat_status_after(size_t writes);
// Reports whether firmware has paused new data/status presentations.
bool mock_ieee_tx_paused(void);
// Models fetching every captured command-channel byte.
void mock_ieee_clear_status(void);
