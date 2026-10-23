// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <optional>

#include "system.h"

// Return the board owned by the sole active firmware session or throw.
System& firmware_system();
// Adjacent CTRL/STATUS samples describe one FIFO head, not the live bus.
struct FirmwareRxSample {
    std::optional<fpga::IeeeStatus> status;
    std::optional<fpga::IeeeControlStatus> control;
};
// Consume pending metadata so other SPI activity cannot reuse stale evidence.
FirmwareRxSample firmware_take_read_status();
// Observe the drive's absolute CTRL/STATUS/RX reads, not generic SPI traffic.
void firmware_record_read(uint32_t address, uint8_t value, FirmwareRxSample previous);
