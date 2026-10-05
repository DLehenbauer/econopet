// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "d64_fixture.h"

namespace disk_fixture {

// Own a formatted, transport-independent 35-track image and its allocation state.
class D64 {
public:
    // Create a named empty filesystem, not an anonymous zero-filled container.
    D64() : image_(DISKIMAGE_D64_SIZE) {
        check(d64_fixture_empty(&layout_, image_.data(), image_.size()));
    }

    // Copies retain independent image storage and construction state.
    D64(const D64&) = default;
    D64& operator=(const D64&) = default;

    // Transfer ownership and explicitly invalidate the source in every library.
    D64(D64&& other) noexcept
        : image_(std::move(other.image_)), layout_(other.layout_),
          corrupted_(other.corrupted_), valid_(other.valid_) {
        other.invalidate();
    }

    // Replace ownership, preserving self-moves and invalidating any other source.
    D64& operator=(D64&& other) noexcept {
        if (this != &other) {
            image_ = std::move(other.image_);
            layout_ = other.layout_;
            corrupted_ = other.corrupted_;
            valid_ = other.valid_;
            other.invalidate();
        }
        return *this;
    }

    // Return an explicitly empty, formatted fixture.
    static D64 empty() { return D64{}; }

    // Encode an ASCII name as uppercase PETSCII, and store payload bytes exactly.
    // No load address is added. Validation failure leaves this fixture unchanged.
    D64& prg(const std::string& name, const std::vector<uint8_t>& payload) {
        return file(DISKIMAGE_FTYPE_PRG, name, payload);
    }

    // Store an ordinary sequential file with the same checked ownership.
    D64& seq(const std::string& name, const std::vector<uint8_t>& payload) {
        return file(DISKIMAGE_FTYPE_SEQ, name, payload);
    }

    // Store a user file with the same checked ownership.
    D64& usr(const std::string& name, const std::vector<uint8_t>& payload) {
        return file(DISKIMAGE_FTYPE_USR, name, payload);
    }

    // Observe immutable bytes for mounting or independent parser assertions.
    const std::vector<uint8_t>& bytes() const { return image_; }

    // Resolve checked D64 coordinates without involving a transport or parser.
    static size_t offset(unsigned int track, unsigned int sector) {
        size_t result;
        check(d64_fixture_offset(track, sector, &result));
        return result;
    }

    // Deliberately replace bytes for malformed-media tests. Corruption prevents
    // further checked file installation, but the image remains mountable as raw.
    D64& corrupt(size_t offset, const std::vector<uint8_t>& bytes) {
        if (!valid_ || image_.size() != DISKIMAGE_D64_SIZE)
            throw std::logic_error("D64 corruption: moved-from fixture");
        if (offset > image_.size() || bytes.size() > image_.size() - offset)
            throw std::out_of_range("D64 corruption: byte range outside image");
        if (bytes.empty()) return *this;
        std::copy(bytes.begin(), bytes.end(), image_.begin() + offset);
        corrupted_ = true;
        return *this;
    }

private:
    // All ordinary file types share validation, encoding and allocation.
    D64& file(unsigned int type, const std::string& name, const std::vector<uint8_t>& payload) {
        require_buildable();
        if (name.find('\0') != std::string::npos)
            throw std::invalid_argument("D64 filename: embedded NUL");
        check(d64_fixture_file(&layout_, image_.data(), image_.size(), type, name.c_str(),
            payload.data(), payload.size(), nullptr, 0));
        return *this;
    }

    // Translate shared C diagnostics into checked authoring failures.
    static void check(const char* error) {
        if (error != nullptr) throw std::invalid_argument(error);
    }

    // Moved-from storage and explicitly corrupted metadata are not buildable.
    void require_buildable() const {
        if (!valid_ || image_.size() != DISKIMAGE_D64_SIZE || corrupted_)
            throw std::logic_error("D64 file: moved-from or corrupted fixture");
    }

    // Make moved-from bytes and allocation state deterministically empty.
    void invalidate() noexcept {
        image_.clear();
        layout_ = {};
        corrupted_ = false;
        valid_ = false;
    }

    std::vector<uint8_t> image_;
    d64_fixture_layout_t layout_{};
    bool corrupted_ = false;
    bool valid_ = true;
};

} // namespace disk_fixture
