// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "framework/asm.h"
#include "framework/trace.h"

namespace test_asm {

namespace phase_detail {
inline constexpr size_t HandshakeSize = 16;
inline constexpr size_t GateLoadOffset = 7;
inline constexpr uint8_t Push6502Flags = 0x08, Push6502Accumulator = 0x48;
inline constexpr uint8_t Pull6502Accumulator = 0x68, Pull6502Flags = 0x28;
inline constexpr uint8_t Compare6502Immediate = 0xc9, Branch6502NotEqual = 0xd0;
inline constexpr uint8_t Push6809SystemStack = 0x34, Pull6809SystemStack = 0x35;
inline constexpr uint8_t Compare6809Immediate = 0x81, Branch6809NotEqual = 0x26;
inline constexpr uint8_t Stack6809ConditionCodes = 0x01, Stack6809Accumulator = 0x02;
inline constexpr uint8_t Saved6809Registers = Stack6809ConditionCodes | Stack6809Accumulator;
// Save A/flags, publish a token, poll the closed release gate, then restore A/flags.
template<class Cpu>
Instruction<Cpu, HandshakeSize> handshake(econopet::CpuAddress acknowledgment,
                                        econopet::CpuAddress release, uint8_t token) {
    static_assert(std::is_same_v<Cpu, Mos6502> || std::is_same_v<Cpu, Mc6809>);
    constexpr uint8_t PollPreviousLoad = 0xf9; // BNE displacement -7
    const auto ack_low = static_cast<uint8_t>(acknowledgment.value());
    const auto ack_high = static_cast<uint8_t>(acknowledgment.value() >> 8);
    const auto release_low = static_cast<uint8_t>(release.value());
    const auto release_high = static_cast<uint8_t>(release.value() >> 8);
    if constexpr (std::is_same_v<Cpu, Mos6502>)
        return {{Push6502Flags, Push6502Accumulator, mos6502::LdaImmediate, token,
            mos6502::StaAbsolute, ack_low, ack_high, mos6502::LdaAbsolute, release_low, release_high,
            Compare6502Immediate, token, Branch6502NotEqual, PollPreviousLoad, Pull6502Accumulator, Pull6502Flags}};
    else
        return {{Push6809SystemStack, Saved6809Registers, mc6809::LdaImmediate, token,
            mc6809::StaExtended, ack_high, ack_low, mc6809::LdaExtended, release_high, release_low,
            Compare6809Immediate, token, Branch6809NotEqual, PollPreviousLoad, Pull6809SystemStack, Saved6809Registers}};
}
} // namespace phase_detail

// A preassembled closed-gate boundary with a unique nonzero acknowledgment token.
struct PausePoint {
    std::string name;
    uint8_t token;
    econopet::CpuAddress gate_load;
};

// A move-only capability issued after a real CPU acknowledgment and closed-gate read.
class PhaseHandle {
public:
    PhaseHandle(const PhaseHandle&) = delete;
    PhaseHandle& operator=(const PhaseHandle&) = delete;
    PhaseHandle(PhaseHandle&&) = default;
    PhaseHandle& operator=(PhaseHandle&&) = default;
    // Borrow the acknowledged boundary's name independently of capability validity.
    const std::string& name() const & { return name_; }
    // Transfer a temporary handle's diagnostic name.
    std::string name() && { return std::move(name_); }
    // Copy an immutable temporary handle's diagnostic name.
    std::string name() const && { return name_; }
    // Identify the permanent terminal boundary, which cannot be resumed.
    bool complete() const { return complete_; }
    // Borrow the phase-entry through acknowledgment interval, excluding host dwell.
    const test_observation::TraceWindow& window() const & { return window_; }
    // Transfer a temporary phase interval.
    test_observation::TraceWindow window() && { return std::move(window_); }
    // Copy an immutable temporary phase interval.
    test_observation::TraceWindow window() const && { return window_; }
private:
    template<class> friend class ProgramBuilder;
    PhaseHandle(std::shared_ptr<const int> owner, size_t index, std::string name, bool complete,
                test_observation::TraceWindow window)
        : owner_(std::move(owner)), index_(index), name_(std::move(name)), complete_(complete),
          window_(std::move(window)) {}
    std::shared_ptr<const int> owner_;
    size_t index_;
    std::string name_;
    bool complete_;
    test_observation::TraceWindow window_;
};
} // namespace test_asm
