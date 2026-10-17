// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include "system.h"
#include "transactions.h"

namespace ieee_test {

// Passive digital wire trace, independent of RX FIFO reads or firmware service.
// Samples a byte on each DAV assertion at the completed-cycle observer phase.
class WireTrace : public test_observation::ScopedRecorder<Record> {
public:
    // Start from the current DAV level, never invent an already-active transfer.
    explicit WireTrace(System& board) : ScopedRecorder(board,
        [released = sample(board).dav_released](const System& observed, auto& state) mutable {
            const auto bus = sample(observed);
            if (released && !bus.dav_released)
                state.append(observed.time(), bus.record);
            released = bus.dav_released;
        }) {}
    WireTrace(const WireTrace&) = delete;
    WireTrace& operator=(const WireTrace&) = delete;
    WireTrace(WireTrace&&) noexcept = default;
    WireTrace& operator=(WireTrace&&) noexcept = default;

private:
    struct BusSample {
        bool dav_released;
        Record record;
    };

    // Resolve device, fitted controller and external levels without double-counting CPU snoops.
    static BusSample sample(const System& board) {
        const auto bus = test_observation::ieee_bus_levels(board);
        return {bus.dav_released,
            raw_record(bus.byte, !bus.atn_released, bus.eoi_released ? Eoi::More : Eoi::Last)};
    }
};

} // namespace ieee_test

// Obtain the scoped wire recorder from the board without exposing the generated model.
inline ieee_test::WireTrace System::trace_ieee() & { return ieee_test::WireTrace(*this); }
