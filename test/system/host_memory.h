// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "types.h"

// Test backing for layout and assembly contracts independent of Verilator.
class HostMemory {
public:
    // Read a physical byte with the same domain boundary as System.
    uint8_t peek(econopet::SramAddress address) const { ++reads; return bytes.at(address.value()); }
    // Record direct fixture writes independently of a CPU bus or clock.
    void poke(econopet::SramAddress address, uint8_t value) { bytes.at(address.value()) = value; ++writes; }
    // Preserve operation-specific failures for simulator-independent tests.
    template<class Exception>
    Exception failure(const std::string& operation, const std::string& detail) const {
        return Exception(operation + ": " + detail);
    }
    std::vector<uint8_t> bytes = std::vector<uint8_t>(econopet::SramCapacity);
    size_t writes = 0;
    mutable size_t reads = 0;
};
