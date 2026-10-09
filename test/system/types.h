// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "hardware_contract.h"

namespace econopet {

inline constexpr uint64_t SramCapacity = uint64_t{1} << ECONOPET_RAM_ADDR_WIDTH;
inline constexpr uint64_t CpuAddressCapacity = uint64_t{1} << ECONOPET_CPU_ADDR_WIDTH;
inline constexpr uint64_t WishboneAddressCapacity = uint64_t{1} << ECONOPET_WB_ADDR_WIDTH;
inline constexpr uint64_t HalfTicksPerCycle = 2;

// Borrow a byte span with checked indexing, never a direct pointer/count pair.
class ByteView {
public:
    // Borrow a mutable or immutable byte span without extending its lifetime.
    template<class Byte, size_t Extent>
        requires std::is_convertible_v<std::span<Byte, Extent>, std::span<const uint8_t>>
    constexpr ByteView(std::span<Byte, Extent> bytes) : bytes_(bytes) {}
    // View a fixed C array for the lifetime of its owner.
    template<size_t Size>
    constexpr ByteView(const uint8_t (&bytes)[Size]) : bytes_(bytes) {}
    // View a fixed standard array, including an empty one.
    template<size_t Size>
    constexpr ByteView(const std::array<uint8_t, Size>& bytes)
        : bytes_(bytes) {}
    // View a dynamic byte buffer without copying or extending its lifetime.
    constexpr ByteView(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    // Return the borrowed buffer's byte count.
    constexpr size_t size() const { return bytes_.size(); }
    // Read a byte only within the borrowed extent.
    constexpr uint8_t operator[](size_t index) const {
        if (index >= bytes_.size()) throw std::out_of_range("byte view index exceeds its extent");
        return bytes_[index];
    }

private:
    std::span<const uint8_t> bytes_;
};

// Bounded address with checked construction and offset arithmetic.
// Integer inputs are explicit and checked before narrowing. Domains never convert.
template<class Domain, uint64_t Capacity>
class Address {
    static_assert(Capacity > 0 && Capacity <= (uint64_t{1} << 32));
public:
    // Initialize address zero for inactive pins and empty fixtures.
    constexpr Address() = default;
    // Check an integer address before storing its hardware encoding.
    template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
    explicit constexpr Address(Integer value) {
        static_assert(sizeof(Integer) <= sizeof(uint64_t));
        if constexpr (std::is_signed_v<Integer>) {
            if (value < 0) throw std::out_of_range(std::string(domain_name())
                + " construction: negative address=" + std::to_string(value));
        }
        if (static_cast<uint64_t>(value) >= Capacity)
            throw std::out_of_range(std::string(domain_name()) + " construction: address="
                + std::to_string(value) + " exceeds capacity=" + std::to_string(Capacity));
        value_ = static_cast<uint32_t>(value);
    }
    // Return the address encoding for pins, byte assembly or C firmware.
    constexpr uint32_t value() const { return value_; }
    // Offset an address without wrapping at either end of the address space.
    template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
    constexpr Address operator+(Integer offset) const {
        static_assert(sizeof(Integer) <= sizeof(uint64_t));
        if constexpr (std::is_signed_v<Integer>) {
            if (offset < 0) {
                const uint64_t magnitude = static_cast<uint64_t>(-(offset + 1)) + 1;
                if (magnitude > value_) throw std::out_of_range(std::string(domain_name())
                    + " offset: address=" + std::to_string(value_) + ", offset="
                    + std::to_string(offset) + " underflows");
                return Address(value_ - magnitude);
            }
        }
        if (static_cast<uint64_t>(offset) >= Capacity - value_)
            throw std::out_of_range(std::string(domain_name()) + " offset: address="
                + std::to_string(value_) + ", offset=" + std::to_string(offset) + " overflows");
        return Address(value_ + static_cast<uint64_t>(offset));
    }
    // Measure a displacement between addresses without unsigned underflow.
    friend constexpr int64_t operator-(Address lhs, Address rhs) {
        return static_cast<int64_t>(lhs.value_) - rhs.value_;
    }
    // Compare addresses within the same domain.
    friend constexpr bool operator==(Address lhs, Address rhs) { return lhs.value_ == rhs.value_; }
    // Compare addresses within the same domain.
    friend constexpr bool operator!=(Address lhs, Address rhs) { return !(lhs == rhs); }

private:
    // Name the checked domain in standalone failures that have no board owner.
    static constexpr const char* domain_name() {
        if constexpr (Capacity == CpuAddressCapacity) return "CpuAddress";
        if constexpr (Capacity == SramCapacity) return "SramAddress";
        if constexpr (Capacity == WishboneAddressCapacity) return "WishboneAddress";
        return "Address";
    }
    uint32_t value_ = 0;
};

struct CpuAddressDomain;
struct SramAddressDomain;
struct WishboneAddressDomain;
using CpuAddress = Address<CpuAddressDomain, CpuAddressCapacity>;
using SramAddress = Address<SramAddressDomain, SramCapacity>;
using WishboneAddress = Address<WishboneAddressDomain, WishboneAddressCapacity>;

// Full system-clock cycles, never PHI2 clocks or half-cycle simulation ticks.
class Cycles {
public:
    // Check an integer duration before it can reach the simulation clock.
    template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
    constexpr Cycles(Integer value) {
        static_assert(sizeof(Integer) <= sizeof(uint64_t));
        if constexpr (std::is_signed_v<Integer>) {
            if (value < 0) throw std::out_of_range("negative cycle duration");
        }
        if (static_cast<uint64_t>(value) > Maximum)
            throw std::out_of_range("cycle duration overflows simulation time");
        value_ = static_cast<uint64_t>(value);
    }

    // Return the full-cycle count for bounded iteration.
    constexpr uint64_t value() const { return value_; }

    // Convert the duration to simulation half ticks without overflow.
    constexpr uint64_t half_ticks() const { return value_ * HalfTicksPerCycle; }

    // Convert the simulator's private half ticks, rejecting incomplete cycles.
    static constexpr Cycles from_half_ticks(uint64_t ticks) {
        if (ticks % HalfTicksPerCycle)
            throw std::logic_error("simulation clock stopped within a full cycle");
        return Cycles{ticks / HalfTicksPerCycle};
    }

    // Add durations without overflowing the simulator's representable range.
    friend constexpr Cycles operator+(Cycles lhs, Cycles rhs) {
        if (rhs.value_ > Maximum - lhs.value_)
            throw std::overflow_error("cycle duration addition overflow");
        return Cycles{lhs.value_ + rhs.value_};
    }

    // Subtract durations without unsigned wraparound.
    friend constexpr Cycles operator-(Cycles lhs, Cycles rhs) {
        if (rhs.value_ > lhs.value_) throw std::out_of_range("cycle duration subtraction underflow");
        return Cycles{lhs.value_ - rhs.value_};
    }

    // Scale a duration by a nonnegative integer without overflowing simulation time.
    template<std::integral Integer>
    friend constexpr Cycles operator*(Cycles duration, Integer count) {
        static_assert(sizeof(Integer) <= sizeof(uint64_t));

        if constexpr (std::is_signed_v<Integer>) {
            if (count < 0) throw std::out_of_range("negative cycle duration multiplier");
        }

        const uint64_t multiplier = static_cast<uint64_t>(count);
        if (multiplier && duration.value_ > Maximum / multiplier) {
            throw std::overflow_error("cycle duration multiplication overflow");
        }

        return Cycles{duration.value_ * multiplier};
    }

    // Compare durations in the same full-cycle unit.
    friend constexpr bool operator==(Cycles lhs, Cycles rhs) { return lhs.value_ == rhs.value_; }

    // Compare durations in the same full-cycle unit.
    friend constexpr bool operator!=(Cycles lhs, Cycles rhs) { return !(lhs == rhs); }

    // Order durations in the same full-cycle unit.
    friend constexpr bool operator<(Cycles lhs, Cycles rhs) { return lhs.value_ < rhs.value_; }

    // Order durations in the same full-cycle unit.
    friend constexpr bool operator>(Cycles lhs, Cycles rhs) { return rhs < lhs; }

    // Order durations in the same full-cycle unit.
    friend constexpr bool operator<=(Cycles lhs, Cycles rhs) { return !(rhs < lhs); }

    // Order durations in the same full-cycle unit.
    friend constexpr bool operator>=(Cycles lhs, Cycles rhs) { return !(lhs < rhs); }

    // Include units in assertion diagnostics.
    friend std::ostream& operator<<(std::ostream& stream, Cycles duration) {
        return stream << duration.value_ << " cycles";
    }

    static constexpr uint64_t Maximum = std::numeric_limits<uint64_t>::max() / HalfTicksPerCycle;

private:
    uint64_t value_ = 0;
};

// A board clock timestamp, distinct from a duration but in the same full cycles.
class CycleTime {
public:
    // Construct a timestamp from full cycles elapsed since simulation startup.
    explicit constexpr CycleTime(Cycles elapsed) : elapsed_(elapsed) {}

    // Return the full-cycle timestamp for diagnostics.
    constexpr uint64_t value() const { return elapsed_.value(); }

    // Construct a deadline with checked timestamp arithmetic.
    friend constexpr CycleTime operator+(CycleTime start, Cycles duration) {
        return CycleTime{start.elapsed_ + duration};
    }

    // Measure elapsed full cycles between ordered timestamps.
    friend constexpr Cycles operator-(CycleTime end, CycleTime start) {
        return end.elapsed_ - start.elapsed_;
    }

    // Compare timestamps in the same full-cycle unit.
    friend constexpr bool operator==(CycleTime lhs, CycleTime rhs) { return lhs.elapsed_ == rhs.elapsed_; }

    // Compare timestamps in the same full-cycle unit.
    friend constexpr bool operator!=(CycleTime lhs, CycleTime rhs) { return !(lhs == rhs); }

    // Order timestamps in the same full-cycle unit.
    friend constexpr bool operator<(CycleTime lhs, CycleTime rhs) { return lhs.elapsed_ < rhs.elapsed_; }

    // Order timestamps in the same full-cycle unit.
    friend constexpr bool operator>(CycleTime lhs, CycleTime rhs) { return rhs < lhs; }

    // Include timestamp units in assertion diagnostics.
    friend std::ostream& operator<<(std::ostream& stream, CycleTime timestamp) {
        return stream << "cycle " << timestamp.value();
    }

private:
    Cycles elapsed_;
};

enum class CpuControlBit : uint8_t {
    Ready = ECONOPET_REG_CPU_READY_MASK,
    Reset = ECONOPET_REG_CPU_RESET_MASK,
    Nmi = ECONOPET_REG_CPU_NMI_MASK,
    All = Ready | Reset | Nmi,
};

// Checked bit sets cannot mix unrelated flag domains or retain unknown bits.
template<class Bit>
class Flags {
public:
    // Initialize an empty bit set.
    constexpr Flags() = default;

    // Initialize one named flag or a validated composite enum value.
    constexpr Flags(Bit bit) : Flags(from_bits(std::to_underlying(bit))) {}

    // Reject unknown or non-byte register bits before converting them into a typed set.
    static constexpr Flags from_bits(uint64_t bits) {
        if (bits > std::numeric_limits<uint8_t>::max())
            throw std::invalid_argument("board flag set exceeds byte capacity");
        if (bits & ~static_cast<uint64_t>(std::to_underlying(Bit::All)))
            throw std::invalid_argument("unknown bits in board flag set");
        Flags result;
        result.bits_ = static_cast<uint8_t>(bits);
        return result;
    }

    // Return the wire encoding at a byte-oriented boundary.
    constexpr uint8_t bits() const { return bits_; }

    // Test whether every requested bit is present.
    constexpr bool contains(Bit bit) const {
        return contains(Flags(bit));
    }

    // Test a composite mask without mixing flag domains.
    constexpr bool contains(Flags requested) const {
        return (bits_ & requested.bits_) == requested.bits_;
    }

    // Combine sets belonging to the same flag domain.
    friend constexpr Flags operator|(Flags lhs, Flags rhs) { return from_bits(lhs.bits_ | rhs.bits_); }

    // Compare sets belonging to the same flag domain.
    friend constexpr bool operator==(Flags lhs, Flags rhs) { return lhs.bits_ == rhs.bits_; }

    // Compare sets belonging to the same flag domain.
    friend constexpr bool operator!=(Flags lhs, Flags rhs) { return !(lhs == rhs); }

private:
    uint8_t bits_ = 0;
};

// Combine named flags without implicitly enabling bitwise operations on concepts.
template<class Bit, class = decltype(Bit::All)>
constexpr Flags<Bit> operator|(Bit lhs, Bit rhs) {
    return Flags<Bit>(lhs) | Flags<Bit>(rhs);
}

using CpuControl = Flags<CpuControlBit>;

} // namespace econopet
