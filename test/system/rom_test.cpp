// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "system.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "framework/system_test.h"

namespace {
using namespace econopet;
constexpr SramAddress Probe{0x1234};
constexpr SramAddress LastByte{SramCapacity - 1};
constexpr uint8_t Marker = 0x42;
constexpr uint8_t OtherMarker = 0x73;

// Restore the caller's media environment after discovery-independent resolution tests.
class MediaEnvironment {
public:
    // Retain the original value, including the distinction between absent and empty.
    MediaEnvironment() {
        if (const char* value = std::getenv("ECONOPET_MEDIA_DIR")) saved_ = value;
    }
    // Restore process state and report failures rather than silently leaking it.
    ~MediaEnvironment() {
        const int result = saved_ ? ::setenv("ECONOPET_MEDIA_DIR", saved_->c_str(), 1)
                                  : ::unsetenv("ECONOPET_MEDIA_DIR");
        EXPECT_EQ(result, 0) << "cannot restore media environment";
    }
    // Set or remove the variable without performing the operation inside an assertion.
    void set(const char* value) const {
        const int result = value ? ::setenv("ECONOPET_MEDIA_DIR", value, 1)
                                 : ::unsetenv("ECONOPET_MEDIA_DIR");
        if (result != 0) throw std::runtime_error("cannot change media environment");
    }
private:
    std::optional<std::string> saved_;
};

// Own synthetic images without sharing paths or requiring proprietary ROM media.
class RomFiles {
public:
    // Allocate a unique directory for this fixture's files.
    RomFiles() {
        const auto pattern = (std::filesystem::temp_directory_path() / "econopet-rom-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const char* created = ::mkdtemp(name.data());
        if (!created) throw std::runtime_error("cannot create temporary ROM directory");
        directory = created;
    }
    // Surface cleanup failures through the test framework.
    ~RomFiles() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        if (error) ADD_FAILURE() << "cannot remove temporary ROM directory: " << error.message();
    }
    RomFiles(const RomFiles&) = delete;
    RomFiles& operator=(const RomFiles&) = delete;

    // Write an exact-size distinguishable image, checking write and close failures.
    std::filesystem::path write(const std::filesystem::path& name, size_t size, uint8_t byte) const {
        const auto path = directory / name;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const std::vector<uint8_t> bytes(size, byte);
        if (!file || (size && !file.write(reinterpret_cast<const char*>(bytes.data()),
                                         static_cast<std::streamsize>(size))))
            throw std::runtime_error("cannot write synthetic ROM: " + path.string());
        file.close();
        if (!file) throw std::runtime_error("cannot close synthetic ROM: " + path.string());
        return path;
    }
    // Populate every declared mapping with synthetic bytes, not copyrighted ROM content.
    void write_set(System::RomSet set, uint8_t byte, const std::filesystem::path& subdirectory = {}) const {
        for (const auto& image : test_rom::images(set))
            write(subdirectory / image.filename, image.size, byte++);
    }
    std::filesystem::path directory;
};

// Compare the entire physical memory, including locations outside incoming mappings.
std::vector<uint8_t> sram_image(const System& board) {
    std::vector<uint8_t> bytes(System::RamSize);
    for (size_t index = 0; index < bytes.size(); ++index) bytes[index] = board.peek(SramAddress{index});
    return bytes;
}

static_assert(std::is_same_v<decltype(std::declval<const System&>().selected_rom()),
                             const std::optional<test_rom::Metadata>&>);
static_assert(std::is_same_v<decltype(std::declval<const System&&>().selected_rom()),
                             std::optional<test_rom::Metadata>>);
}

TEST_F(SystemTest, RomBatchFailuresPreserveBytesOwnershipAndTime) {
    RomFiles files;
    const auto first = files.write("first.bin", 2, Marker);
    const auto later = files.write("later.bin", 1, OtherMarker);
    const auto before = sram_image(system);
    const auto start = system.time();
    std::vector<System::RomImage> images{{first, Probe, 2}, {files.directory / "missing", Probe + 2, 2}};
    expect_failure<std::runtime_error>(system, "install_rom_images",
        [&] { system.install_rom_images(images); }, "cannot open ROM");
    EXPECT_EQ(sram_image(system), before);
    images[1].path = later;
    expect_failure<std::runtime_error>(system, "install_rom_images",
        [&] { system.install_rom_images(images); }, "incorrect ROM size");
    EXPECT_EQ(sram_image(system), before);
    files.write("later.bin", 3, OtherMarker);
    EXPECT_THROW(system.install_rom_images(images), std::runtime_error);
    EXPECT_EQ(sram_image(system), before);
    images[1].path = files.directory;
    EXPECT_THROW(system.install_rom_images(images), std::runtime_error);
    EXPECT_EQ(sram_image(system), before);
    images[1].path = later;
    files.write("later.bin", 2, OtherMarker);
    images[1].address = LastByte;
    EXPECT_THROW(system.install_rom_images(images), std::out_of_range);
    EXPECT_EQ(sram_image(system), before);
    images[1].address = Probe + 2;
    system.install_rom_images(images);
    EXPECT_EQ(system.peek(Probe), Marker);
    EXPECT_EQ(system.peek(Probe + 3), OtherMarker);
    EXPECT_FALSE(system.selected_rom());
    EXPECT_EQ(system.time(), start);
}

TEST_F(SystemTest, RomImagesCoverExactSramExtentWithoutAcceptingEmptyOrOverflowRanges) {
    RomFiles files;
    const auto empty = files.write("empty.bin", 0, 0);
    const auto before = sram_image(system);
    EXPECT_THROW(system.load_rom(empty, LastByte, 0), std::invalid_argument);
    EXPECT_THROW(system.install_rom_images({}), std::invalid_argument);
    EXPECT_THROW(system.load_rom(empty, LastByte, SIZE_MAX), std::out_of_range);
    EXPECT_EQ(sram_image(system), before);
    const auto start = system.time();
    system.load_rom(files.write("full.bin", System::RamSize, Marker), SramAddress{0}, System::RamSize);
    EXPECT_EQ(sram_image(system), std::vector<uint8_t>(System::RamSize, Marker));
    system.load_rom(files.write("last.bin", 1, OtherMarker), LastByte, 1, System::FixtureOverlap::Replace);
    EXPECT_EQ(system.peek(LastByte), OtherMarker);
    EXPECT_EQ(system.peek(SramAddress{LastByte.value() - 1}), Marker);
    EXPECT_EQ(system.time(), start);
}

TEST_F(SystemTest, RomOverlapRequiresReplacementButNeverAllowsOverlappingBatchImages) {
    RomFiles files;
    const auto path = files.write("image.bin", 4, Marker);
    const auto small = files.write("small.bin", 1, OtherMarker);
    system.load_rom(path, Probe, 4);
    const auto before = sram_image(system);
    for (const auto address : {Probe, Probe + 1, Probe + 3, SramAddress{Probe.value() - 1}})
        EXPECT_THROW(system.load_rom(path, address, 4), std::invalid_argument);
    EXPECT_THROW(system.load_rom(files.write("cover.bin", 6, OtherMarker), SramAddress{Probe.value() - 1}, 6),
        std::invalid_argument);
    EXPECT_THROW(system.load_rom(path, Probe + 4, 4, static_cast<System::FixtureOverlap>(99)),
        std::invalid_argument);
    for (const auto start : {Probe, Probe + 10}) {
        EXPECT_THROW(system.install_rom_images({{path, start, 4}, {small, start + 3, 1}},
            System::FixtureOverlap::Replace), std::invalid_argument);
    }
    EXPECT_EQ(sram_image(system), before);
    system.load_rom(small, Probe + 1, 1, System::FixtureOverlap::Replace);
    EXPECT_EQ(system.peek(Probe), Marker);
    EXPECT_EQ(system.peek(Probe + 1), OtherMarker);
    system.load_rom(path, Probe + 4, 4);
    EXPECT_EQ(system.peek(Probe + 7), Marker);
}

TEST_F(SystemTest, RomSetMetadataCommitsWithBytesAndSurvivesDeliberatePatches) {
    RomFiles files;
    files.write_set(System::RomSet::Upgrade, 0x10);
    const auto images = test_rom::images(System::RomSet::Upgrade);
    files.write(images.back().filename, images.back().size - 1, OtherMarker);
    const auto before = sram_image(system);
    EXPECT_THROW(system.load_rom_set(System::RomSet::Upgrade, files.directory), std::runtime_error);
    EXPECT_EQ(sram_image(system), before);
    EXPECT_FALSE(system.selected_rom());
    files.write_set(System::RomSet::Upgrade, 0x10);
    const auto start = system.time();
    system.load_rom_set(System::RomSet::Upgrade, files.directory);
    EXPECT_EQ(system.time(), start);
    ASSERT_TRUE(system.selected_rom());
    const auto saved = *system.selected_rom();
    EXPECT_EQ(saved.set, System::RomSet::Upgrade);
    EXPECT_EQ(saved.cpu, CPU_SOFT_6502);
    EXPECT_STREQ(saved.identity, "PET upgrade ROM 2");
    ASSERT_NE(saved.commodore, nullptr);
    EXPECT_EQ(saved.commodore->Cint, CpuAddress{0xe1de});
    EXPECT_EQ(saved.waterloo, nullptr);
    for (size_t index = 0; index < images.size(); ++index)
        for (size_t offset = 0; offset < images[index].size; ++offset)
            EXPECT_EQ(system.peek(images[index].address + offset), 0x10 + index);
    system.poke(SramAddress{0xffc1}, 0);
    EXPECT_EQ(system.selected_rom()->commodore->Cint, saved.commodore->Cint);
    system.cpu().assert_reset();
    EXPECT_EQ(system.selected_rom()->set, saved.set);
    const auto patched = sram_image(system);
    files.write_set(System::RomSet::Rom4, 0x20);
    const auto rom4 = test_rom::images(System::RomSet::Rom4);
    files.write(rom4.back().filename, 1, 0);
    EXPECT_THROW(system.load_rom_set(System::RomSet::Rom4, files.directory, System::FixtureOverlap::Replace),
        std::runtime_error);
    EXPECT_EQ(sram_image(system), patched);
    EXPECT_EQ(system.selected_rom()->set, saved.set);
    files.write_set(System::RomSet::Rom4, 0x20);
    EXPECT_THROW(system.load_rom_set(System::RomSet::Rom4, files.directory), std::invalid_argument);
    EXPECT_EQ(sram_image(system), patched);
    system.load_rom_set(System::RomSet::Rom4, files.directory, System::FixtureOverlap::Replace);
    EXPECT_EQ(system.selected_rom()->set, System::RomSet::Rom4);
    EXPECT_EQ(system.selected_rom()->commodore->Cint, CpuAddress{0xe000});
    files.write_set(System::RomSet::Waterloo, 0x30);
    system.load_rom_set(System::RomSet::Waterloo, files.directory, System::FixtureOverlap::Replace);
    EXPECT_EQ(system.selected_rom()->cpu, CPU_SOFT_6809);
    ASSERT_NE(system.selected_rom()->waterloo, nullptr);
    EXPECT_EQ(system.selected_rom()->waterloo->IeeeInit, CpuAddress{0xc188});
    EXPECT_EQ(system.selected_rom()->commodore, nullptr);
    EXPECT_EQ(saved.set, System::RomSet::Upgrade);
    const auto detached = std::move(system).selected_rom();
    ASSERT_TRUE(detached);
    EXPECT_EQ(detached->set, System::RomSet::Waterloo);
    EXPECT_STREQ(detached->identity, "Waterloo revision 12");
}

TEST_F(SystemTest, RomMediaDirectoryResolvesOnlyWhenRequested) {
    MediaEnvironment environment;
    RomFiles first;
    RomFiles second;
    const auto before = sram_image(system);
    const auto start = system.time();
    for (const char* value : {static_cast<const char*>(nullptr), ""}) {
        environment.set(value);
        expect_failure<std::runtime_error>(system, "ROM directory", [&] {
            system.load_rom_set(System::RomSet::Upgrade);
        }, "ECONOPET_MEDIA_DIR");
        EXPECT_EQ(sram_image(system), before);
        EXPECT_EQ(system.time(), start);
    }
    std::filesystem::create_directory(first.directory / "roms");
    std::filesystem::create_directory(second.directory / "roms");
    first.write_set(System::RomSet::Upgrade, Marker, "roms");
    second.write_set(System::RomSet::Rom4, OtherMarker, "roms");
    environment.set(first.directory.c_str());
    system.load_rom_set(System::RomSet::Upgrade);
    EXPECT_EQ(system.peek(SramAddress{0xc000}), Marker);
    environment.set(second.directory.c_str());
    system.load_rom_set(System::RomSet::Rom4, System::FixtureOverlap::Replace);
    EXPECT_EQ(system.peek(SramAddress{0xb000}), OtherMarker);
    EXPECT_EQ(system.time(), start);
    EXPECT_TRUE(system.snapshot().cpu_reset_active_o);
    EXPECT_EQ(system.cpu().peek_selection(), CPU_PHYS_6502);
}

TEST_F(SystemTest, GenericRomReplacementInvalidatesOnlyOverwrittenSelectedIdentity) {
    RomFiles files;
    files.write_set(System::RomSet::Upgrade, 0x10);
    system.load_rom_set(System::RomSet::Upgrade, files.directory);
    const auto path = files.write("custom.bin", 1, OtherMarker);
    system.load_rom(path, Probe, 1);
    ASSERT_TRUE(system.selected_rom());
    const auto before = sram_image(system);
    expect_failure<std::invalid_argument>(system, "load_rom_set", [&] {
        system.load_rom_set(static_cast<System::RomSet>(99), files.directory);
    });
    EXPECT_EQ(sram_image(system), before);
    EXPECT_EQ(system.selected_rom()->set, System::RomSet::Upgrade);
    system.load_rom(path, SramAddress{0xffc1}, 1, System::FixtureOverlap::Replace);
    EXPECT_FALSE(system.selected_rom());
    EXPECT_THROW(system.load_rom(path, SramAddress{0xffc2}, 1), std::invalid_argument);
}

TEST_F(SystemTest, RomInstallationRejectsRunningCpuAndCapturedMutations) {
    RomFiles files;
    const auto path = files.write("image.bin", 1, Marker);
    system.cpu(CPU_SOFT_6502).start();
    const auto before = sram_image(system);
    const auto start = system.time();
    EXPECT_THROW(system.load_rom(path, Probe, 1), std::logic_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_EQ(sram_image(system), before);
    system.cpu().assert_reset();
    const auto verify = [&] {
        const auto at = system.time();
        EXPECT_THROW(system.load_rom(path, Probe, 1), std::logic_error);
        EXPECT_THROW(system.install_rom_images({{path, Probe, 1}}), std::logic_error);
        EXPECT_THROW(system.load_rom_set(System::RomSet::Upgrade, files.directory), std::logic_error);
        EXPECT_EQ(system.time(), at);
    };
    auto subscription = system.observe([&](const System&) { verify(); });
    system.tick(1);
    subscription.reset();
    system.run_until([&](const System&) { verify(); return true; }, 0, "ROM observation guard");
    system.raw_stimulus([&](auto&) { verify(); });
    system.io().via().inputs([&](auto&) { verify(); });
    system.load_rom(path, Probe, 1);
    EXPECT_EQ(system.peek(Probe), Marker);
}

TEST_F(SystemTest, RomInstallationCannotRaceAnOwnedOrFailedSpiTransaction) {
    RomFiles files;
    const auto path = files.write("image.bin", 1, OtherMarker);
    const auto before = sram_image(system);
    const auto address = fpga::address(Probe);
    const std::array<uint8_t, 4> write{
        fpga::command_byte(fpga::SpiCommand::WriteAt, address),
        static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value()), Marker};
    system.spi().transaction([&](auto& commands) {
        commands.command(write);
        const auto start = system.time();
        EXPECT_THROW(system.load_rom(path, Probe, 1), std::logic_error);
        EXPECT_EQ(system.time(), start);
        EXPECT_EQ(sram_image(system), before);
    });
    EXPECT_EQ(system.peek(Probe), Marker);
    {
        auto owner = system.spi().raw_transaction();
        owner.bit(true);
    }
    const auto stopped = system.time();
    EXPECT_THROW(system.load_rom(path, Probe, 1), std::logic_error);
    EXPECT_EQ(system.time(), stopped);
    EXPECT_EQ(system.peek(Probe), Marker);
    EXPECT_FALSE(system.selected_rom());
}

TEST_F(SystemTest, RomInstallationRejectsUnclockedExternalResetRelease) {
    RomFiles files;
    files.write_set(System::RomSet::Upgrade, Marker);
    const auto path = files.write("replacement.bin", 1, OtherMarker);
    for (const bool raw : {false, true}) {
        SCOPED_TRACE(raw);
        System board;
        board.load_rom_set(System::RomSet::Upgrade, files.directory);
        // Leave the external pin as the sole reset source.
        board.set_external_reset(true);
        board.tick(1);
        board.spi().write(fpga::Register::CpuControl, CpuControl{CpuControlBit::Ready}.bits());
        ASSERT_TRUE(board.cpu().peek_reset());
        if (raw) board.raw_stimulus([](auto& pins) { pins.cpu_reset_n_i = true; });
        else board.set_external_reset(false);
        ASSERT_TRUE(board.cpu().peek_reset()); // Last evaluated reset is intentionally stale.
        const auto before = sram_image(board);
        const auto start = board.time();
        expect_failure<std::logic_error>(board, "install_rom_images", [&] {
            board.load_rom(path, SramAddress{0xc000}, 1, System::FixtureOverlap::Replace);
        });
        EXPECT_EQ(sram_image(board), before);
        ASSERT_TRUE(board.selected_rom());
        EXPECT_EQ(board.selected_rom()->set, System::RomSet::Upgrade);
        expect_failure<std::logic_error>(board, "install_rom_images", [&] {
            board.load_rom_set(System::RomSet::Upgrade, files.directory, System::FixtureOverlap::Replace);
        });
        EXPECT_EQ(sram_image(board), before);
        EXPECT_EQ(board.time(), start);
        ASSERT_TRUE(board.selected_rom());
        EXPECT_EQ(board.selected_rom()->set, System::RomSet::Upgrade);
        board.tick(1);
        EXPECT_FALSE(board.cpu().peek_reset());
        EXPECT_THROW(board.load_rom(path, Probe, 1), std::logic_error);
        board.set_external_reset(true);
        board.tick(1);
        board.load_rom(path, Probe, 1);
        EXPECT_EQ(board.peek(Probe), OtherMarker);
    }
}

TEST_F(SystemTest, RomResetSettlementRequiresACompletedTickButNotUnchangedEdits) {
    RomFiles files;
    const auto path = files.write("image.bin", 1, Marker);
    // No-op reset edits, unrelated edits and rejected detached edits do not invalidate settlement.
    system.set_external_reset(false);
    system.raw_stimulus([](auto& pins) { pins.diag_i = !pins.diag_i; });
    EXPECT_THROW(system.raw_stimulus([](auto& pins) {
        pins.cpu_reset_n_i = false;
        throw std::runtime_error("discard reset edit");
    }), std::runtime_error);
    system.load_rom(path, Probe, 1);
    for (const bool raw : {false, true}) {
        SCOPED_TRACE(raw);
        // Changing a pin twice still requires evaluation even if its original value is restored.
        if (raw) system.raw_stimulus([](auto& pins) { pins.cpu_reset_n_i = false; });
        else system.set_external_reset(true);
        system.set_external_reset(false);
        system.tick(0);
        const auto before = sram_image(system);
        const auto start = system.time();
        expect_failure<std::logic_error>(system, "install_rom_images", [&] {
            system.load_rom(path, Probe + 1, 1, System::FixtureOverlap::Replace);
        }, "completed tick");
        EXPECT_EQ(sram_image(system), before);
        EXPECT_EQ(system.time(), start);
        system.tick(1);
        system.load_rom(path, Probe + 1, 1, System::FixtureOverlap::Replace);
        EXPECT_EQ(system.peek(Probe + 1), Marker);
    }
}

TEST_F(SystemTest, RomInstallationRejectsUnsettledRawSpiReleaseWithoutClocking) {
    RomFiles files;
    const auto path = files.write("image.bin", 1, OtherMarker);
    const auto address = fpga::address(Probe);
    const std::array<uint8_t, 4> write{
        fpga::command_byte(fpga::SpiCommand::WriteAt, address),
        static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value()), Marker};
    system.drive_spi(false, false, false);
    system.tick(2);
    for (const auto byte : write) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const bool data = (byte & (0x80 >> bit)) != 0;
            system.drive_spi(false, false, data);
            system.tick(2);
            system.drive_spi(false, true, data);
            system.tick(2);
        }
        system.drive_spi(false, false, false);
        system.tick(2);
    }
    system.drive_spi(true, false, false);
    const auto before = sram_image(system);
    const auto start = system.time();
    EXPECT_THROW(system.load_rom(path, Probe, 1), std::logic_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_EQ(sram_image(system), before);
    system.tick(4);
    EXPECT_THROW(system.load_rom(path, Probe, 1), std::logic_error);
    system.run_until([](const System& board) {
        return board.snapshot().spi_quiescent_o;
    }, 10000, "raw ROM write drain");
    EXPECT_EQ(system.peek(Probe), Marker);
    system.load_rom(path, Probe, 1);
    EXPECT_EQ(system.peek(Probe), OtherMarker);
}
