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
        [released = board.snapshot().ieee_bus_dav_no](const System& observed, auto& state) mutable {
            const auto pins = observed.snapshot();
            if (released && !pins.ieee_bus_dav_no)
                state.append(observed.time(), raw_record(static_cast<uint8_t>(~pins.ieee_bus_dio_o),
                    !pins.ieee_bus_atn_no,
                    observed.io().pia1().ca2() && pins.ieee_device_eoi_no ? Eoi::More : Eoi::Last));
            released = pins.ieee_bus_dav_no;
        }) {}
    WireTrace(const WireTrace&) = delete;
    WireTrace& operator=(const WireTrace&) = delete;
    WireTrace(WireTrace&&) noexcept = default;
    WireTrace& operator=(WireTrace&&) noexcept = default;
};

} // namespace ieee_test

// Obtain the scoped wire recorder from the board without exposing the generated model.
inline ieee_test::WireTrace System::trace_ieee() & { return ieee_test::WireTrace(*this); }
