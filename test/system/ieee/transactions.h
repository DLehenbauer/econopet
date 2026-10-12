// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <cstdint>
#include <iomanip>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ieee/ieee_protocol.h"

namespace ieee_test {

enum class Eoi { More, Last, Unobserved };
enum class CommandCategory { Listen, Talk, Secondary, Open, Close, Unlisten, Untalk };

namespace detail {
struct CommandFactory;
}

// Primary address 31 is reserved for UNLISTEN/UNTALK, not a device.
class Device {
public:
    explicit constexpr Device(uint64_t value) : value_(value) {
        if (value > 30) throw std::out_of_range("IEEE device: expected 0..30");
    }
    // Return the validated primary address without command bits.
    constexpr uint8_t value() const { return static_cast<uint8_t>(value_); }
private:
    uint64_t value_;
};

// Commodore OPEN/CLOSE channels occupy four bits.
class Channel {
public:
    explicit constexpr Channel(uint64_t value) : value_(value) {
        if (value > 15) throw std::out_of_range("IEEE channel: expected 0..15");
    }
    // Return the validated Commodore channel.
    constexpr uint8_t value() const { return static_cast<uint8_t>(value_); }
private:
    uint64_t value_;
};

// IEEE secondary addresses occupy five bits, unlike OPEN/CLOSE channels.
class SecondaryAddress {
public:
    explicit constexpr SecondaryAddress(uint64_t value) : value_(value) {
        if (value > 31) throw std::out_of_range("IEEE secondary address: expected 0..31");
    }
    // Return the validated secondary address.
    constexpr uint8_t value() const { return static_cast<uint8_t>(value_); }
private:
    uint64_t value_;
};

class Command {
public:
    // Inspect the command category independently of its wire encoding.
    constexpr CommandCategory category() const { return category_; }
    // Return the exact command byte used on DIO.
    constexpr uint8_t byte() const { return byte_; }
private:
    friend struct detail::CommandFactory;
    // Only typed factories may pair a category with its shared protocol encoding.
    constexpr Command(CommandCategory category, uint8_t byte) : category_(category), byte_(byte) {}
    CommandCategory category_;
    uint8_t byte_;
};

namespace detail {
struct CommandFactory {
    // Assemble an already validated command without revalidating its operand.
    static constexpr Command make(CommandCategory category, uint8_t byte) {
        return Command{category, byte};
    }
};
}

// Construct named command values from their checked address domains.
inline constexpr Command Listen(Device device) {
    return detail::CommandFactory::make(CommandCategory::Listen, IEEE_CMD_LISTEN(device.value()));
}
// Construct a talker command from a checked primary address.
inline constexpr Command Talk(Device device) {
    return detail::CommandFactory::make(CommandCategory::Talk, IEEE_CMD_TALK(device.value()));
}
// Construct an OPEN command from a checked Commodore channel.
inline constexpr Command Open(Channel channel) {
    return detail::CommandFactory::make(CommandCategory::Open, IEEE_CMD_OPEN(channel.value()));
}
// Construct a CLOSE command from a checked Commodore channel.
inline constexpr Command Close(Channel channel) {
    return detail::CommandFactory::make(CommandCategory::Close, IEEE_CMD_CLOSE(channel.value()));
}
// Construct a secondary command from a checked five-bit address.
inline constexpr Command Secondary(SecondaryAddress address) {
    return detail::CommandFactory::make(CommandCategory::Secondary, IEEE_CMD_SECONDARY(address.value()));
}
// A Commodore channel is also a valid IEEE secondary address.
inline constexpr Command Secondary(Channel channel) {
    return detail::CommandFactory::make(CommandCategory::Secondary, IEEE_CMD_SECONDARY(channel.value()));
}
inline constexpr Command Unlisten = detail::CommandFactory::make(CommandCategory::Unlisten, IEEE_CMD_UNLISTEN);
inline constexpr Command Untalk = detail::CommandFactory::make(CommandCategory::Untalk, IEEE_CMD_UNTALK);

// Detached wire observation. ATN and EOI mean asserted, not electrical levels.
struct Record {
    uint8_t data;
    bool atn;
    Eoi eoi;

    // Require the complete byte/ATN/EOI tuple for equality.
    friend bool operator==(const Record& lhs, const Record& rhs) {
        return lhs.data == rhs.data && lhs.atn == rhs.atn && lhs.eoi == rhs.eoi;
    }
};

// Explicit escape for unusual command flags and malformed protocol sequences.
inline Record raw_record(uint8_t byte, bool atn, Eoi eoi) {
    if (eoi != Eoi::More && eoi != Eoi::Last)
        throw std::invalid_argument("IEEE record: unsupported EOI policy");
    return {byte, atn, eoi};
}
// Expected command records always assert ATN and release EOI.
inline Record command(Command value) { return raw_record(value.byte(), true, Eoi::More); }
// Expected payload records release ATN, with explicit final-byte EOI.
inline Record data(uint8_t byte, Eoi eoi = Eoi::More) { return raw_record(byte, false, eoi); }

// Explicitly discard EOI when expecting the RX FIFO's byte/ATN-only format.
inline Record fifo_record(Record record) {
    record.eoi = Eoi::Unobserved;
    return record;
}

// Build RX expectations while visibly opting out of unsupported EOI evidence.
inline std::vector<Record> fifo_records(std::vector<Record> records) {
    for (auto& record : records) record = fifo_record(record);
    return records;
}

// Print all wire fields for standalone diagnostics and GoogleTest matchers.
inline void PrintTo(const Record& record, std::ostream* stream) {
    std::ostringstream text;
    text << "byte=0x" << std::hex << std::setw(2) << std::setfill('0')
         << unsigned(record.data) << " ATN=" << record.atn
         << " EOI=";
    if (record.eoi == Eoi::Unobserved) text << "unobserved";
    else if (record.eoi == Eoi::More || record.eoi == Eoi::Last) text << (record.eoi == Eoi::Last);
    else text << "invalid(" << std::dec << static_cast<int>(record.eoi) << ')';
    *stream << text.str();
}
// Format without changing the caller's stream state.
inline std::string describe(const Record& record) {
    std::ostringstream text;
    PrintTo(record, &text);
    return text.str();
}

// Compare captured sequences without accessing a board or consuming any FIFO.
inline std::optional<std::string> sequence_mismatch(
    const std::vector<Record>& actual, const std::vector<Record>& expected) {
    const size_t common = actual.size() < expected.size() ? actual.size() : expected.size();
    for (size_t index = 0; index < common; ++index) {
        if (!(actual[index] == expected[index]))
            return "index=" + std::to_string(index) + ": expected {" + describe(expected[index])
                + "}, actual {" + describe(actual[index]) + "}";
    }
    if (actual.size() < expected.size())
        return "index=" + std::to_string(common) + ": expected {" + describe(expected[common])
            + "}, actual {missing}";
    if (actual.size() > expected.size())
        return "index=" + std::to_string(common) + ": expected {end}, actual {"
            + describe(actual[common]) + "}";
    return std::nullopt;
}

// Require exact detached records, including length and every flag.
inline void require_sequence(const std::vector<Record>& actual,
                             const std::vector<Record>& expected, const std::string& reason) {
    if (const auto mismatch = sequence_mismatch(actual, expected))
        throw std::runtime_error("IEEE sequence: " + reason + ": " + *mismatch);
}

} // namespace ieee_test
