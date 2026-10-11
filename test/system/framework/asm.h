// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "framework/memory.h"
#include "types.h"

namespace test_asm {

enum class RegionKind { Code, Stack, Mailbox, Results, Vectors, Data, Workspace };

template<class Memory> class Scenario;

// A reservation can only be obtained from a checked scenario layout.
class Region {
public:
    // Return the first CPU address in this reservation.
    econopet::CpuAddress start() const { return start_; }
    // Return the byte count, including a possible final byte at $FFFF.
    size_t size() const { return size_; }
    // Return an address within the reservation, never its one-past endpoint.
    econopet::CpuAddress at(size_t offset) const {
        if (offset >= size_) throw std::out_of_range("region at: offset=" + std::to_string(offset)
            + " exceeds " + name_ + ", CPU address=" + std::to_string(start_.value())
            + ", size=" + std::to_string(size_));
        return start_ + offset;
    }
    // Narrow an existing reservation without creating a new overlapping owner.
    Region slice(size_t offset, size_t size) const {
        if (!memory_detail::contains_extent(size_, offset, size))
            throw std::out_of_range("region slice: offset=" + std::to_string(offset)
                + ", requested=" + std::to_string(size) + " exceeds " + name_
                + ", CPU address=" + std::to_string(start_.value()) + ", size=" + std::to_string(size_));
        return Region(owner_, name_, kind_, at(offset), size);
    }

private:
    template<class> friend class Scenario;
    Region(std::shared_ptr<const int> owner, std::string name, RegionKind kind,
           econopet::CpuAddress start, size_t size)
        : owner_(std::move(owner)), name_(std::move(name)), kind_(kind), start_(start), size_(size) {}
    std::shared_ptr<const int> owner_;
    std::string name_;
    RegionKind kind_;
    econopet::CpuAddress start_;
    size_t size_;
};

// Record fixture-owned CPU memory before installing code or data.
// Memory supplies peek/poke and failure diagnostics (System or a host fixture).
template<class Memory>
class Scenario {
public:
    // Borrow persistent explicit lower-bank backing, not live CPU banking or I/O.
    explicit Scenario(Memory& memory) : memory_(memory) {}
    Scenario(Memory&&) = delete;
    Scenario(const Scenario&) = delete;
    Scenario& operator=(const Scenario&) = delete;
    Scenario(Scenario&&) = default;
    Scenario& operator=(Scenario&&) = delete;

    // Attach backing-memory evidence to rejected layout operations without clocks.
    template<class Exception>
    Exception failure(const std::string& operation, const std::string& detail) const {
        return memory_.template failure<Exception>(operation, detail);
    }
    // Reserve a named nonempty region, rejecting every overlap and CPU-space wrap.
    Region reserve(std::string name, RegionKind kind, econopet::CpuAddress start, size_t size) {
        require_live();
        if (name.empty()) throw failure<std::invalid_argument>("reserve", "fixture reservation needs a name");
        switch (kind) {
        case RegionKind::Code: case RegionKind::Stack: case RegionKind::Mailbox:
        case RegionKind::Results: case RegionKind::Vectors: case RegionKind::Data:
        case RegionKind::Workspace:
            break;
        default:
            throw failure<std::invalid_argument>("reserve", "unknown fixture region purpose");
        }
        if (size == 0 || size > econopet::CpuAddressCapacity - start.value())
            throw failure<std::out_of_range>("reserve", "fixture reservation exceeds CPU address space: "
                + name + ", address=" + std::to_string(start.value()) + ", size=" + std::to_string(size));
        for (const auto& region : regions_) {
            if (region.name_ == name)
                throw failure<std::invalid_argument>("reserve", "duplicate fixture reservation: " + name);
            if (start.value() < region.start_.value() + region.size_
                && region.start_.value() < start.value() + size)
                throw failure<std::invalid_argument>("reserve", "fixture reservation " + name + " overlaps " + region.name_);
        }
        Region region(owner_, std::move(name), kind, start, size);
        regions_.push_back(region);
        return region;
    }
    // Check reservation identity and purpose before installation or data access.
    void require(const Region& region, RegionKind kind) const {
        require_owned(region);
        if (region.kind_ != kind)
            throw failure<std::invalid_argument>("require region", "fixture region has the wrong purpose");
    }
    // Bind host data operations to ordinary data in this reservation's lower bank.
    MemoryView<Memory> lower_bank(const Region& region) const {
        require_owned(region);
        if (region.kind_ != RegionKind::Data && region.kind_ != RegionKind::Results
            && region.kind_ != RegionKind::Workspace)
            throw failure<std::invalid_argument>("lower bank", "requires an ordinary data reservation");
        return MemoryView<Memory>(memory_, owner_, region.name_, region.start(), region.size());
    }
    // Resolve a named reservation without introducing an overlapping alias.
    Region region(const std::string& name) const {
        require_live();
        for (const auto& region : regions_)
            if (region.name_ == name) return region;
        throw failure<std::invalid_argument>("region", "unknown reservation " + name);
    }
    // Check data-operand identity before fixture or later instruction operations.
    void require(const MemoryByte& byte) const {
        require_live();
        if (byte.owner_ != owner_)
            throw failure<std::invalid_argument>("memory operand", "foreign byte " + byte.name()
                + ", address=" + std::to_string(byte.address().value()));
    }
    // Install a bounded byte span without CPU decoding, SPI or clock advancement.
    void write(const Region& region, size_t offset, econopet::ByteView bytes) {
        require_owned(region);
        if (offset > region.size_ || bytes.size() > region.size_ - offset)
            throw failure<std::out_of_range>("scenario write", "fixture write exceeds " + region.name_
                + ", address=" + std::to_string(region.start().value()) + ", offset=" + std::to_string(offset)
                + ", size=" + std::to_string(bytes.size()));
        for (size_t i = 0; i < bytes.size(); ++i)
            memory_.poke(econopet::SramAddress{region.at(offset + i).value()}, bytes[i]);
    }

private:
    // Reject all layout operations after transferring its identity to another owner.
    void require_live() const {
        if (!owner_) throw failure<std::logic_error>("layout", "fixture scenario was moved from");
    }
    // Check region identity independently of its purpose.
    void require_owned(const Region& region) const {
        require_live();
        if (region.owner_ != owner_)
            throw failure<std::invalid_argument>("memory region", "foreign or moved-from reservation");
    }
    Memory& memory_;
    std::shared_ptr<const int> owner_ = std::make_shared<const int>(0);
    std::vector<Region> regions_;
};

} // namespace test_asm
