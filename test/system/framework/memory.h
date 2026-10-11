// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "types.h"

namespace test_asm {

namespace memory_detail {
// Check a nonempty subextent without overflowing endpoint arithmetic.
constexpr bool contains_extent(size_t extent, size_t offset, size_t size) {
    return offset < extent && size != 0 && size <= extent - offset;
}
}

enum class ByteOrder { LittleEndian, BigEndian };
template<class Memory> class MemoryView;

// Owned data operands retain CPU addresses, not backing SRAM addresses.
class MemoryByte {
public:
    // Returns the explicit CPU operand for low-level encoders.
    econopet::CpuAddress address() const { return address_; }
    // Returns the diagnostic name retained after the layout is destroyed.
    const std::string& name() const & { return name_; }
    // Transfer a temporary operand's diagnostic name.
    std::string name() && { return std::move(name_); }
    // Copy an immutable temporary operand's diagnostic name.
    std::string name() const && { return name_; }
private:
    template<class> friend class MemoryView;
    friend class MemoryBuffer;
    friend class MemoryCapture;
    friend class MemoryWord;
    template<class> friend class Scenario;
    MemoryByte(std::shared_ptr<const int> owner, econopet::CpuAddress address, std::string name)
        : owner_(std::move(owner)), address_(address), name_(std::move(name)) {}
    std::shared_ptr<const int> owner_;
    econopet::CpuAddress address_;
    std::string name_;
};

class MemoryWord {
public:
    // Returns the first CPU address of the two-byte word.
    econopet::CpuAddress address() const { return first_.address(); }
    // Selects storage order explicitly, independently of the host architecture.
    ByteOrder order() const { return order_; }
    // Returns a checked constituent byte suitable for CPU instructions.
    MemoryByte byte(size_t index) const {
        if (index >= 2) throw std::out_of_range("memory word: byte index=" + std::to_string(index));
        return MemoryByte(first_.owner_, first_.address_ + index, first_.name_);
    }
private:
    template<class> friend class MemoryView;
    friend class MemoryBuffer;
    friend class MemoryCapture;
    MemoryWord(MemoryByte first, ByteOrder order) : first_(std::move(first)), order_(order) {
        if (order != ByteOrder::LittleEndian && order != ByteOrder::BigEndian)
            throw std::invalid_argument("memory word: unknown byte order");
    }
    MemoryByte first_;
    ByteOrder order_;
};

class MemoryBuffer {
public:
    // Returns the first CPU address without converting address domains.
    econopet::CpuAddress address() const { return first_.address(); }
    // Returns the nonempty owned extent.
    size_t size() const { return size_; }
    // Selects one checked byte and preserves ownership.
    MemoryByte byte(size_t offset) const {
        if (offset >= size_) throw std::out_of_range("memory buffer " + first_.name_
            + ": byte offset=" + std::to_string(offset) + ", size=" + std::to_string(size_));
        return MemoryByte(first_.owner_, first_.address_ + offset, first_.name_);
    }
    // Selects a complete checked word, never a wrapping or partial pointer.
    MemoryWord word(size_t offset, ByteOrder order) const {
        if (!memory_detail::contains_extent(size_, offset, 2))
            throw std::out_of_range("memory buffer " + first_.name_ + ": word exceeds extent");
        return MemoryWord(byte(offset), order);
    }
    // Creates a nonempty owned sub-buffer.
    MemoryBuffer slice(size_t offset, size_t size) const {
        if (!memory_detail::contains_extent(size_, offset, size))
            throw std::out_of_range("memory buffer " + first_.name_ + ": slice exceeds extent");
        return MemoryBuffer(byte(offset), size);
    }
private:
    template<class> friend class MemoryView;
    friend class MemoryCapture;
    MemoryBuffer(MemoryByte first, size_t size) : first_(std::move(first)), size_(size) {}
    MemoryByte first_;
    size_t size_;
};

// Detached captures reject foreign owners and data not selected for capture.
class MemoryCapture {
public:
    // Looks up the same owned byte used by an instruction or fixture operation.
    uint8_t operator[](const MemoryByte& byte) const {
        return bytes_[require_extent(byte, 1)];
    }
    // Decodes an owned word using its declared storage order.
    uint16_t operator[](const MemoryWord& word) const {
        const auto offset = require_extent(word.first_, 2);
        const auto a = bytes_[offset], b = bytes_[offset + 1];
        return word.order() == ByteOrder::LittleEndian ? a | (b << 8) : (a << 8) | b;
    }
    // Copies a checked buffer out of the immutable capture.
    std::vector<uint8_t> operator[](const MemoryBuffer& buffer) const {
        const auto offset = require_extent(buffer.first_, buffer.size());
        return std::vector<uint8_t>(bytes_.begin() + offset, bytes_.begin() + offset + buffer.size());
    }
    // Exposes the primary raw extent only for explicit low-level framework checks.
    const std::vector<uint8_t>& raw_bytes() const & { require_live(); return bytes_; }
    // Transfer raw bytes out of a temporary capture.
    std::vector<uint8_t> raw_bytes() && {
        require_live();
        auto bytes = std::move(bytes_);
        owner_.reset();
        return bytes;
    }
    // Copy raw bytes out of an immutable temporary capture.
    std::vector<uint8_t> raw_bytes() const && { require_live(); return bytes_; }
private:
    template<class> friend class MemoryView;
    MemoryCapture(std::shared_ptr<const int> owner, econopet::CpuAddress start, std::vector<uint8_t> bytes)
        : owner_(std::move(owner)), start_(start), bytes_(std::move(bytes)) {}
    // Validate the complete requested capture extent before copying any data.
    size_t require_extent(const MemoryByte& first, size_t size) const {
        require_live();
        if (first.owner_ != owner_)
            throw std::invalid_argument("memory capture: foreign byte " + first.name_
                + ", CPU address=" + std::to_string(first.address_.value()));
        if (first.address_.value() < start_.value()
            || !memory_detail::contains_extent(bytes_.size(), first.address_.value() - start_.value(), size))
            throw std::invalid_argument("memory capture: missing byte " + first.name_
                + ", CPU address=" + std::to_string(first.address_.value()) + ", size=" + std::to_string(size));
        return first.address_.value() - start_.value();
    }
    // Transferred captures must not report successful detached evidence.
    void require_live() const {
        if (!owner_) throw std::logic_error("memory capture: data was transferred");
    }
    std::shared_ptr<const int> owner_;
    econopet::CpuAddress start_;
    std::vector<uint8_t> bytes_;
};

// A layout-bound view selects explicit lower-bank backing once.
template<class Memory>
class MemoryView {
public:
    // Creates a named byte within this view.
    MemoryByte byte(std::string name, size_t offset = 0) const {
        return buffer(std::move(name), offset, 1).byte(0);
    }
    // Creates a named two-byte word with explicit byte order.
    MemoryWord word(std::string name, size_t offset, ByteOrder order) const {
        return MemoryWord(buffer(std::move(name), offset, 2).byte(0), order);
    }
    // Creates a named buffer wholly contained in the owning reservation.
    MemoryBuffer buffer(std::string name, size_t offset, size_t size) const {
        validate_name(name);
        const auto selected = extent_.slice(offset, size);
        return MemoryBuffer(MemoryByte(extent_.first_.owner_, selected.address(), std::move(name)), size);
    }
    // Reads host backing memory without CPU decoding, SPI or clocks.
    uint8_t read(const MemoryByte& byte) const {
        require(byte);
        return memory_.peek(econopet::SramAddress{byte.address().value()});
    }
    // Reads a pointer or scalar using its declared byte order.
    uint16_t read(const MemoryWord& word) const {
        require_extent(word.first_, 2);
        const auto first = econopet::SramAddress{word.address().value()};
        const auto a = memory_.peek(first), b = memory_.peek(first + 1);
        return word.order() == ByteOrder::LittleEndian ? a | (b << 8) : (a << 8) | b;
    }
    // Copies checked host backing bytes.
    std::vector<uint8_t> read(const MemoryBuffer& buffer) const {
        require(buffer);
        std::vector<uint8_t> bytes(buffer.size());
        const auto first = econopet::SramAddress{buffer.address().value()};
        for (size_t i = 0; i < buffer.size(); ++i) bytes[i] = memory_.peek(first + i);
        return bytes;
    }
    // Snapshot the entire view into detached, layout-identified host-owned data.
    MemoryCapture capture() const {
        return MemoryCapture(extent_.first_.owner_, extent_.address(), read(extent_));
    }
    // Initializes fixture backing only, never simulating CPU-generated writes.
    template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
    void initialize(const MemoryByte& byte, Integer value) {
        require(byte);
        memory_.poke(econopet::SramAddress{byte.address().value()},
            static_cast<uint8_t>(checked_value(value, 0xff, byte)));
    }
    // Initializes both pointer bytes after validating the entire destination.
    template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
    void initialize(const MemoryWord& word, Integer value) {
        require_extent(word.first_, 2);
        const auto checked = checked_value(value, 0xffff, word.first_);
        const auto low = static_cast<uint8_t>(checked), high = static_cast<uint8_t>(checked >> 8);
        const auto first = econopet::SramAddress{word.address().value()};
        memory_.poke(first, word.order() == ByteOrder::LittleEndian ? low : high);
        memory_.poke(first + 1, word.order() == ByteOrder::LittleEndian ? high : low);
    }
    // Initializes a CPU pointer without losing its address type.
    void initialize(const MemoryWord& word, econopet::CpuAddress value) { initialize(word, value.value()); }
    // Initializes an exact buffer after checking ownership and length before mutation.
    void initialize(const MemoryBuffer& buffer, econopet::ByteView bytes) {
        require(buffer);
        if (bytes.size() != buffer.size())
            throw memory_.template failure<std::out_of_range>("memory initialize", "buffer length mismatch");
        const auto first = econopet::SramAddress{buffer.address().value()};
        for (size_t i = 0; i < bytes.size(); ++i) memory_.poke(first + i, bytes[i]);
    }
private:
    template<class> friend class Scenario;
    MemoryView(Memory& memory, std::shared_ptr<const int> owner, std::string name,
               econopet::CpuAddress start, size_t size)
        : memory_(memory), extent_(MemoryByte(std::move(owner), start, std::move(name)), size) {}
    // Rejects signed underflow and narrowing before any backing writes.
    template<class Integer>
    uint64_t checked_value(Integer value, uint64_t maximum, const MemoryByte& byte) const {
        static_assert(sizeof(Integer) <= sizeof(uint64_t));
        const auto detail = [&] {
            return byte.name() + ", CPU address=" + std::to_string(byte.address().value())
                + ", value=" + std::to_string(value);
        };
        if constexpr (std::is_signed_v<Integer>) {
            if (value < 0)
                throw memory_.template failure<std::out_of_range>("memory initialize", "negative " + detail());
        }
        if (static_cast<uint64_t>(value) > maximum)
            throw memory_.template failure<std::out_of_range>("memory initialize", "storage width exceeded: " + detail());
        return static_cast<uint64_t>(value);
    }
    // Rejects empty diagnostic names before constructing a handle.
    void validate_name(const std::string& name) const {
        if (!extent_.first_.owner_)
            throw memory_.template failure<std::logic_error>("memory view", "view was moved from");
        if (name.empty()) throw memory_.template failure<std::invalid_argument>("memory handle", "empty name");
    }
    // Checks both layout identity and containment before host access.
    void require(const MemoryByte& byte) const {
        require_extent(byte, 1);
    }
    // Validate ownership and the complete nonempty extent once before host access.
    void require_extent(const MemoryByte& first, size_t size) const {
        if (!extent_.first_.owner_)
            throw memory_.template failure<std::logic_error>("memory view", "view was moved from");
        if (first.owner_ != extent_.first_.owner_ || first.address().value() < extent_.address().value()
            || !memory_detail::contains_extent(extent_.size(), first.address().value() - extent_.address().value(), size))
            throw memory_.template failure<std::invalid_argument>("memory view", "foreign byte " + first.name_);
    }
    // Validates the full buffer before any host writes.
    void require(const MemoryBuffer& buffer) const {
        require_extent(buffer.first_, buffer.size());
    }
    Memory& memory_;
    MemoryBuffer extent_;
};

} // namespace test_asm
