// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include "transactions.h"
#include "framework/observations.h"

namespace ieee_test {

// Active SPI sample only. This neither acknowledges a head nor promises future emptiness.
inline bool rx_empty_now(System& board) {
    return !board.ieee().read_status().contains(fpga::IeeeStatusBit::RxReady);
}

// Discard transport-only boundaries, then peek at the next bus byte without popping it.
inline std::optional<Record> peek_rx(System& board) {
    constexpr size_t BoundaryLimit = 32;
    for (size_t skipped = 0; skipped <= BoundaryLimit; ++skipped) {
        const auto status = board.ieee().read_status();
        if (!status.contains(fpga::IeeeStatusBit::RxReady)) return std::nullopt;
        const auto control = board.ieee().read_control();
        if (control.contains(fpga::IeeeControlStatusBit::RxAtnEnd)) {
            board.spi().write(fpga::Register::IeeeRx, 0);
            continue;
        }
        const auto byte = board.spi().read(fpga::Register::IeeeRx);
        return Record{byte, status.contains(fpga::IeeeStatusBit::RxAtn),
            control.contains(fpga::IeeeControlStatusBit::RxEoi) ? Eoi::Last : Eoi::More};
    }
    throw board.failure<std::runtime_error>("IEEE RX", "ATN boundaries exceed one FIFO capacity");
}

// Actively read and acknowledge one bus byte, excluding transport-only boundaries.
inline std::optional<Record> take_rx(System& board) {
    const auto record = peek_rx(board);
    if (record) board.spi().write(fpga::Register::IeeeRx, 0);
    return record;
}

// Drain through physical SPI until an empty status sample, under one deadline.
// Concurrent producers may run during SPI. Limit and deadline bound that work.
inline std::vector<Record> drain_rx(System& board, size_t limit, econopet::Cycles budget,
                                    const std::string& reason) {
    if (limit == 0)
        throw board.failure<std::invalid_argument>("IEEE drain", reason + ": record limit must be positive");
    std::vector<Record> records;
    bool empty = false;
    board.service_until([&](const System&) { return empty; },
        [&](System& active, econopet::Cycles) {
            if (records.size() == limit) {
                if (peek_rx(active))
                    throw active.failure<std::runtime_error>("IEEE drain",
                        reason + ": record limit at index=" + std::to_string(limit));
                empty = true;
            } else if (auto record = take_rx(active)) {
                records.push_back(*record);
            } else {
                empty = true;
            }
        }, budget, "IEEE drain: " + reason);
    return records;
}

// Actively drain and compare through the first mismatch or an empty status sample.
// A failed check may leave later records pending. This is not a quiet-interval assertion.
inline void require_drained_sequence(System& board, const std::vector<Record>& expected,
                                     econopet::Cycles budget, const std::string& reason) {
    std::vector<Record> actual;
    bool empty = false;
    board.service_until([&](const System&) { return empty; },
        [&](System& active, econopet::Cycles) {
            if (const auto record = take_rx(active)) {
                const size_t index = actual.size();
                actual.push_back(*record);
                if (index < expected.size() && *record == expected[index]) return;
            } else {
                empty = true;
            }
            if (const auto mismatch = sequence_mismatch(actual, expected))
                throw active.failure<std::runtime_error>("IEEE sequence", reason + ": " + *mismatch);
        }, budget, "IEEE sequence drain: " + reason);
}

// Observe both endpoints and every completed cycle without SPI or acknowledgment.
// A scheduled consumer cannot hide a head seen at an earlier cycle boundary.
inline void require_rx_remains_empty(System& board, econopet::Cycles interval,
                                     const std::string& reason) {
    test_observation::require_absent(board,
        [](const System& observed) { return observed.snapshot().ieee_rx_ready_o; },
        interval, "IEEE RX remains empty: " + reason);
}

} // namespace ieee_test
