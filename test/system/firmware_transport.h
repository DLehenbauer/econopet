// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include "system.h"

// Return the board owned by the sole active firmware session or throw.
System& firmware_system();
// Consume pending semantic status so other SPI activity cannot reuse stale evidence.
fpga::IeeeStatus firmware_take_read_status();
// Observe the drive's absolute status/RX read pair, not generic SPI traffic.
void firmware_record_read(uint32_t address, uint8_t value, fpga::IeeeStatus previous_status);
