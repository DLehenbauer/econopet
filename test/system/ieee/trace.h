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
        const auto pins = board.snapshot();
        const auto& io = board.io();
        const auto pia1_inputs = io.pia1().peek_inputs();
        const auto pia2_inputs = io.pia2().peek_inputs();
        const auto via_inputs = io.via().peek_inputs();
        // Input-mode GPIO pins release the bus regardless of retained output latches.
        const auto& pia2_port = io.pia2().state().pb;
        const auto pia2_outputs = static_cast<uint8_t>(pia2_port.outr | ~pia2_port.ddr);
        const auto& via_port = io.via().state().pb;
        const auto via_outputs = static_cast<uint8_t>(via_port.outr | ~via_port.ddr);
        const bool dav_released = pins.ieee_device_dav_no && io.pia2().cb2()
            && (via_inputs.port_b & pet::Dav) != 0;
        const bool atn_released = pia2_inputs.ca1
            && (via_outputs & pet::Atn) != 0;
        const bool eoi_released = pins.ieee_device_eoi_no && io.pia1().ca2()
            && (pia1_inputs.port_a & pet::EoiInput) != 0;
        const auto byte = static_cast<uint8_t>(
            ~(pins.ieee_device_dio_o & pia2_outputs & pia2_inputs.port_a));
        return {dav_released, raw_record(byte, !atn_released, eoi_released ? Eoi::More : Eoi::Last)};
    }
};

} // namespace ieee_test

// Obtain the scoped wire recorder from the board without exposing the generated model.
inline ieee_test::WireTrace System::trace_ieee() & { return ieee_test::WireTrace(*this); }
