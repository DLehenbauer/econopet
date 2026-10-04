// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include <array>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "d64.h"

namespace {
using disk_fixture::D64;

// Decode generated chain bytes independently of the production disk parser.
std::vector<uint8_t> payload(const D64& disk, unsigned int file) {
    const auto& image = disk.bytes();
    const size_t entry = D64::offset(18, 1 + file / 8) + (file % 8) * 32;
    unsigned int track = image.at(entry + 3);
    unsigned int sector = image.at(entry + 4);
    std::set<size_t> visited;
    std::vector<uint8_t> result;
    do {
        const size_t offset = D64::offset(track, sector);
        if (!visited.insert(offset).second) throw std::runtime_error("fixture chain cycle");
        const size_t length = image.at(offset) == 0 ? image.at(offset + 1) - 1u : 254u;
        if (length > 254) throw std::runtime_error("fixture invalid terminal length");
        result.insert(result.end(), image.begin() + offset + 2, image.begin() + offset + 2 + length);
        track = image.at(offset);
        sector = image.at(offset + 1);
    } while (track != 0);
    return result;
}

// Compare every BAM count and bit against independent chain/directory ownership.
void require_bam(const D64& disk) {
    const auto& bytes = disk.bytes();
    const size_t bam = D64::offset(18, 0);
    std::set<size_t> occupied{bam};
    unsigned int directory_track = 18;
    unsigned int directory_sector = 1;
    do {
        const size_t directory = D64::offset(directory_track, directory_sector);
        ASSERT_TRUE(occupied.insert(directory).second);
        for (unsigned int entry = 0; entry < 8; ++entry) {
            const size_t offset = directory + entry * 32;
            if (bytes[offset + 2] == 0) continue;
            unsigned int track = bytes[offset + 3];
            unsigned int sector = bytes[offset + 4];
            do {
                const size_t data = D64::offset(track, sector);
                ASSERT_TRUE(occupied.insert(data).second);
                track = bytes[data];
                sector = bytes[data + 1];
            } while (track != 0);
        }
        directory_track = bytes[directory];
        directory_sector = bytes[directory + 1];
    } while (directory_track != 0);
    for (unsigned int track = 1; track <= 35; ++track) {
        const unsigned int sectors = track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
        unsigned int free = 0;
        for (unsigned int sector = 0; sector < 24; ++sector) {
            const bool available = (bytes[bam + track * 4 + 1 + sector / 8] & (1u << (sector % 8))) != 0;
            if (sector < sectors) {
                free += available;
                EXPECT_EQ(available, occupied.count(D64::offset(track, sector)) == 0);
            } else EXPECT_FALSE(available);
        }
        EXPECT_EQ(bytes[bam + track * 4], free) << "track " << track;
    }
}
} // namespace

// A named empty fixture is formatted, with no files and correct free-space bits.
TEST(DiskFixture, EmptyIsFormattedWithoutDirectoryEntries) {
    const auto disk = D64::empty();
    ASSERT_EQ(disk.bytes().size(), 174848u);
    const size_t bam = D64::offset(18, 0);
    const size_t dir = D64::offset(18, 1);
    EXPECT_EQ(disk.bytes()[bam], 18);
    EXPECT_EQ(disk.bytes()[bam + 1], 1);
    EXPECT_EQ(disk.bytes()[bam + 2], 'A');
    EXPECT_EQ(disk.bytes()[bam + 18 * 4], 17);
    EXPECT_EQ(disk.bytes()[bam + 18 * 4 + 1], 0xfc);
    EXPECT_EQ(disk.bytes()[dir], 0);
    EXPECT_EQ(disk.bytes()[dir + 1], 0xff);
    EXPECT_EQ(disk.bytes()[dir + 2], 0);
    unsigned int free_data = 0;
    for (unsigned int track = 1; track <= 35; ++track)
        if (track != 18) free_data += disk.bytes()[bam + track * 4];
    EXPECT_EQ(free_data, 664u);
    require_bam(disk);
}

// Directory encoding is closed PRG, uppercase PETSCII, padding and block count.
TEST(DiskFixture, PrgOwnsNamePayloadAndDirectoryMetadata) {
    D64 disk;
    EXPECT_EQ(&disk.prg("basic", {0x42, 0x43, 0x44, 0x45}), &disk);
    const size_t dir = D64::offset(18, 1);
    const auto& bytes = disk.bytes();
    EXPECT_EQ(bytes[dir + 2], 0x82);
    EXPECT_EQ(std::string(bytes.begin() + dir + 5, bytes.begin() + dir + 10), "BASIC");
    for (size_t i = 10; i < 21; ++i) EXPECT_EQ(bytes[dir + i], 0xa0);
    EXPECT_EQ(bytes[dir + 30], 1);
    EXPECT_EQ(bytes[dir + 31], 0);
    EXPECT_EQ(payload(disk, 0), (std::vector<uint8_t>{0x42, 0x43, 0x44, 0x45}));
    const size_t data = D64::offset(bytes[dir + 3], bytes[dir + 4]);
    EXPECT_EQ(bytes[data], 0);
    EXPECT_EQ(bytes[data + 1], 5);
    const size_t bam = D64::offset(18, 0);
    EXPECT_EQ(bytes[bam + 17 * 4], 20);
    EXPECT_EQ(bytes[bam + 17 * 4 + 1], 0xfe);
    require_bam(disk);
}

// Ordinary file types share allocation, with only the directory type differing.
TEST(DiskFixture, SequentialAndUserFilesShareCheckedEncoding) {
    D64 disk;
    disk.seq("TEXT", {'A', 'B'}).usr("USER", {0xff}).prg("CODE", {1});
    const size_t dir = D64::offset(18, 1);
    EXPECT_EQ(disk.bytes()[dir + 2], 0x81);
    EXPECT_EQ(disk.bytes()[dir + 32 + 2], 0x83);
    EXPECT_EQ(disk.bytes()[dir + 64 + 2], 0x82);
    EXPECT_EQ(payload(disk, 0), (std::vector<uint8_t>{'A', 'B'}));
    EXPECT_EQ(payload(disk, 1), (std::vector<uint8_t>{0xff}));
    EXPECT_EQ(payload(disk, 2), (std::vector<uint8_t>{1}));
    require_bam(disk);
}

// Exact multiples must use terminal offset 255, never an extra empty sector.
TEST(DiskFixture, PayloadLengthsPreserveExactSectorBoundaries) {
    for (const size_t length : {0u, 1u, 253u, 254u, 255u, 508u, 509u, 6000u}) {
        SCOPED_TRACE(length);
        std::vector<uint8_t> expected(length);
        for (size_t i = 0; i < length; ++i) expected[i] = static_cast<uint8_t>(i);
        D64 disk;
        disk.prg("PAYLOAD", expected);
        EXPECT_EQ(payload(disk, 0), expected);
        const size_t dir = D64::offset(18, 1);
        EXPECT_EQ(disk.bytes()[dir + 30], length == 0 ? 1u : (length + 253) / 254);
        require_bam(disk);
    }
}

// Directory expansion must preserve the first entry and chain all 18 sectors.
TEST(DiskFixture, DirectoryExpansionAndFullDirectoryAreChecked) {
    D64 disk;
    for (unsigned int i = 0; i < D64_FIXTURE_FILES; ++i)
        disk.prg("FILE" + std::to_string(i), {static_cast<uint8_t>(i)});
    for (unsigned int i = 0; i < D64_FIXTURE_FILES; ++i)
        EXPECT_EQ(payload(disk, i), (std::vector<uint8_t>{static_cast<uint8_t>(i)}));
    for (unsigned int sector = 1; sector <= 18; ++sector) {
        const size_t offset = D64::offset(18, sector);
        EXPECT_EQ(disk.bytes()[offset], sector == 18 ? 0 : 18);
        EXPECT_EQ(disk.bytes()[offset + 1], sector == 18 ? 0xff : sector + 1);
    }
    EXPECT_EQ(disk.bytes()[D64::offset(18, 0) + 18 * 4], 0);
    const auto before = disk.bytes();
    EXPECT_THROW(disk.prg("EXTRA", {1}), std::invalid_argument);
    EXPECT_EQ(disk.bytes(), before);
    require_bam(disk);
}

// Both a maximum-size file and disk exhaustion are checked before mutation.
TEST(DiskFixture, CapacityAndDiskFullFailuresAreAtomic) {
    D64 disk;
    const std::vector<uint8_t> maximum(664u * 254u, 0x5a);
    EXPECT_THROW(disk.prg("OVERSIZE", std::vector<uint8_t>(maximum.size() + 1)), std::invalid_argument);
    disk.prg("FULL", maximum);
    EXPECT_EQ(payload(disk, 0), maximum);
    const size_t dir = D64::offset(18, 1);
    EXPECT_EQ(disk.bytes()[dir + 30], 0x98);
    EXPECT_EQ(disk.bytes()[dir + 31], 2);
    const auto before = disk.bytes();
    EXPECT_THROW(disk.prg("EXTRA", {1}), std::invalid_argument);
    EXPECT_EQ(disk.bytes(), before);
    require_bam(disk);
}

// Filename failures retain bytes and allocation state, including normalized duplicates.
TEST(DiskFixture, InvalidAndDuplicateNamesLeaveFixtureReusable) {
    D64 disk;
    disk.prg("BASIC", {1});
    const auto before = disk.bytes();
    for (const auto& name : std::vector<std::string>{"", std::string(17, 'A'), "A:B", "A,B",
             "A=B", "A*", "A?", "A\"", "\n", std::string(1, '\xa0'), std::string("A\0B", 3), "basic"}) {
        EXPECT_THROW(disk.prg(name, {2}), std::invalid_argument) << name;
        EXPECT_EQ(disk.bytes(), before);
    }
    disk.prg(std::string(16, 'N'), {2});
    EXPECT_EQ(payload(disk, 1), (std::vector<uint8_t>{2}));
}

// Reuse production's pure PETSCII mapping, without its firmware transport.
TEST(DiskFixture, FilenamePunctuationUsesSharedPetsciiEncoding) {
    D64 disk;
    disk.prg("a_b", {1});
    const size_t dir = D64::offset(18, 1);
    EXPECT_EQ(disk.bytes()[dir + 5], 'A');
    EXPECT_EQ(disk.bytes()[dir + 6], 0xa4);
    EXPECT_EQ(disk.bytes()[dir + 7], 'B');
}

// Explicit corruption is bounded and does not masquerade as checked authoring.
TEST(DiskFixture, CorruptionIsExplicitBoundedAndStopsCheckedInstallation) {
    D64 disk;
    const auto before = disk.bytes();
    EXPECT_THROW(disk.corrupt(174848, {1}), std::out_of_range);
    EXPECT_THROW(disk.corrupt(std::numeric_limits<size_t>::max(), {1}), std::out_of_range);
    EXPECT_EQ(disk.bytes(), before);
    disk.corrupt(174848, {});
    disk.prg("BASIC", {1});
    disk.corrupt(D64::offset(18, 1), {36, 0});
    EXPECT_EQ(disk.bytes()[D64::offset(18, 1)], 36);
    EXPECT_THROW(disk.prg("NEXT", {2}), std::logic_error);
}

// Checked coordinates span every zone and reject the image-end off-by-one.
TEST(DiskFixture, GeometryRejectsInvalidTrackAndSector) {
    EXPECT_EQ(D64::offset(1, 0), 0u);
    EXPECT_EQ(D64::offset(18, 0), 17u * 21u * 256u);
    EXPECT_EQ(D64::offset(25, 0), (17u * 21u + 7u * 19u) * 256u);
    EXPECT_EQ(D64::offset(31, 0), (17u * 21u + 7u * 19u + 6u * 18u) * 256u);
    EXPECT_EQ(D64::offset(35, 16), 174848u - 256u);
    EXPECT_THROW(D64::offset(0, 0), std::invalid_argument);
    EXPECT_THROW(D64::offset(36, 0), std::invalid_argument);
    EXPECT_THROW(D64::offset(17, 21), std::invalid_argument);
    EXPECT_THROW(D64::offset(24, 19), std::invalid_argument);
    EXPECT_THROW(D64::offset(30, 18), std::invalid_argument);
    EXPECT_THROW(D64::offset(35, 17), std::invalid_argument);
}

// Copies own independent construction state. Moving cannot leave a writable zombie.
TEST(DiskFixture, CopyAndMovePreserveIndependentOwnership) {
    D64 first;
    first.prg("FIRST", {1});
    auto copy = first;
    copy.prg("SECOND", {2});
    first.prg("OTHER", {3});
    EXPECT_EQ(payload(copy, 1), (std::vector<uint8_t>{2}));
    EXPECT_EQ(payload(first, 1), (std::vector<uint8_t>{3}));
    auto moved = std::move(copy);
    EXPECT_EQ(payload(moved, 1), (std::vector<uint8_t>{2}));
    EXPECT_THROW(copy.prg("ZOMBIE", {4}), std::logic_error);
    EXPECT_THROW(copy.corrupt(0, {1}), std::logic_error);
}

// C callers get diagnostics and unchanged storage/state for every preflight failure.
TEST(DiskFixture, SharedCBuilderValidatesStoragePointersAndExplicitChains) {
    std::vector<uint8_t> image(DISKIMAGE_D64_SIZE, 0x5a);
    d64_fixture_layout_t layout{};
    const uint8_t byte = 42;
    EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size(), "A", &byte, 1, nullptr, 0), nullptr);
    EXPECT_NE(d64_fixture_empty(&layout, image.data(), image.size() - 1), nullptr);
    EXPECT_NE(d64_fixture_empty(nullptr, image.data(), image.size()), nullptr);
    EXPECT_NE(d64_fixture_empty(&layout, nullptr, image.size()), nullptr);
    EXPECT_EQ(image.front(), 0x5a);
    ASSERT_EQ(d64_fixture_empty(&layout, image.data(), image.size()), nullptr);
    const auto before = image;
    const std::array<d64_fixture_sector_t, 2> repeated{{{17, 0}, {17, 0}}};
    const std::vector<uint8_t> two_sectors(255);
    EXPECT_NE(d64_fixture_prg(nullptr, image.data(), image.size(), "A", &byte, 1, nullptr, 0), nullptr);
    EXPECT_NE(d64_fixture_prg(&layout, nullptr, image.size(), "A", &byte, 1, nullptr, 0), nullptr);
    EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size(), nullptr, &byte, 1, nullptr, 0), nullptr);
    EXPECT_NE(d64_fixture_file(&layout, image.data(), image.size(),
        DISKIMAGE_FTYPE_REL, "REL", &byte, 1, nullptr, 0), nullptr);
    EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size(), "A", nullptr, 1, nullptr, 0), nullptr);
    EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size(), "A", &byte, 1, nullptr, 1), nullptr);
    EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size() - 1, "A", &byte, 1, nullptr, 0), nullptr);
    EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size(), "A", &byte, 1,
        repeated.data(), repeated.size()), nullptr);
    EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size(), "A", two_sectors.data(),
        two_sectors.size(), repeated.data(), repeated.size()), nullptr);
    for (const auto sector : std::array<d64_fixture_sector_t, 3>{{{18, 1}, {36, 0}, {17, 21}}}) {
        EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size(), "A", &byte, 1, &sector, 1), nullptr);
    }
    EXPECT_EQ(image, before);
    EXPECT_EQ(layout.files, 0u);
    const std::array<d64_fixture_sector_t, 2> sparse{{{17, 0}, {17, 5}}};
    ASSERT_EQ(d64_fixture_prg(&layout, image.data(), image.size(), "A", two_sectors.data(),
        two_sectors.size(), sparse.data(), sparse.size()), nullptr);
    EXPECT_EQ(image[D64::offset(17, 0)], 17);
    EXPECT_EQ(image[D64::offset(17, 0) + 1], 5);
    const auto installed = image;
    EXPECT_NE(d64_fixture_prg(&layout, image.data(), image.size(), "B", &byte, 1, sparse.data(), 1), nullptr);
    EXPECT_EQ(image, installed);
    EXPECT_EQ(layout.files, 1u);
    std::array<uint8_t, 16> encoded;
    encoded.fill(0x5a);
    EXPECT_NE(d64_fixture_name("INVALID:NAME", encoded.data()), nullptr);
    for (const auto value : encoded) EXPECT_EQ(value, 0x5a);
    size_t offset = 42;
    EXPECT_NE(d64_fixture_offset(36, 0, &offset), nullptr);
    EXPECT_EQ(offset, 42u);
    EXPECT_NE(d64_fixture_offset(1, 0, nullptr), nullptr);
}
