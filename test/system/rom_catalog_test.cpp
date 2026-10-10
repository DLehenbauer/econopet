// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "roms.h"

namespace {
using econopet::CpuAddress;
using econopet::SramAddress;
using test_rom::CommodoreEntries;
using test_rom::Set;
using test_rom::WaterlooEntries;

static_assert(std::is_same_v<decltype(CommodoreEntries::Ciout), CpuAddress>);
static_assert(std::is_same_v<decltype(test_rom::images(Set::Upgrade)), std::span<const test_rom::Image>>);
}

TEST(RomCatalog, ManifestsHaveIndependentPhysicalMappings) {
    const std::array<std::pair<Set, std::vector<test_rom::Image>>, 3> expected{{
        {Set::Upgrade, {
            {"basic-2-c000.901465-01.bin", SramAddress{0xc000}, 0x1000},
            {"basic-2-d000.901465-02.bin", SramAddress{0xd000}, 0x1000},
            {"edit-2-n.901447-24.bin", SramAddress{0xe000}, 0x800},
            {"kernal-2.901465-03.bin", SramAddress{0xf000}, 0x1000}}},
        {Set::Rom4, {
            {"basic-4-b000.901465-23.bin", SramAddress{0xb000}, 0x1000},
            {"basic-4-c000.901465-20.bin", SramAddress{0xc000}, 0x1000},
            {"basic-4-d000.901465-21.bin", SramAddress{0xd000}, 0x1000},
            {"edit-4-n.901447-29.bin", SramAddress{0xe000}, 0x800},
            {"kernal-4.901465-22.bin", SramAddress{0xf000}, 0x1000}}},
        {Set::Waterloo, {
            {"waterloo-a000-bfff.970018-12.bin", SramAddress{0xa000}, 0x2000},
            {"waterloo-c000-dfff.970019-12.bin", SramAddress{0xc000}, 0x2000},
            {"waterloo-e000-ffff-970034-12.bin", SramAddress{0xe000}, 0x2000}}}
    }};
    for (const auto& [set, entries] : expected) {
        SCOPED_TRACE(std::to_underlying(set));
        const auto actual = test_rom::images(set);
        ASSERT_EQ(actual.size(), entries.size());
        for (size_t index = 0; index < actual.size(); ++index) {
            SCOPED_TRACE(index);
            EXPECT_STREQ(actual[index].filename, entries[index].filename);
            EXPECT_EQ(actual[index].address, entries[index].address);
            EXPECT_EQ(actual[index].size, entries[index].size);
        }
    }
}

TEST(RomCatalog, CommodoreMetadataMatchesRevisionLedger) {
    // Independent expectations from the PET callable-entry ledger, including ROM 4 CIOUT.
    struct Expected {
        const char* name;
        CpuAddress CommodoreEntries::* member;
        uint16_t upgrade, rom4;
    };
    constexpr std::array<Expected, 17> entries{{
        {"CINT", &CommodoreEntries::Cint, 0xe1de, 0xe000},
        {"TALK", &CommodoreEntries::Talk, 0xf0b6, 0xf0d2},
        {"LISTN", &CommodoreEntries::Listen, 0xf0ba, 0xf0d5},
        {"LIST1", &CommodoreEntries::List1, 0xf0bc, 0xf0d7},
        {"ISOUR", &CommodoreEntries::Isour, 0xf0ee, 0xf109},
        {"SECND", &CommodoreEntries::Secondary, 0xf128, 0xf143},
        {"SCATN", &CommodoreEntries::Scatn, 0xf12d, 0xf148},
        {"ER001", &CommodoreEntries::Er001, 0xf146, 0xf175},
        {"TKSA", &CommodoreEntries::Tksa, 0xf164, 0xf193},
        {"TKATN", &CommodoreEntries::Tkatn, 0xf169, 0xf198},
        {"CIOUT", &CommodoreEntries::Ciout, 0xf16f, 0xf19e},
        {"UNTLK", &CommodoreEntries::Untalk, 0xf17f, 0xf1ae},
        {"UNLSN", &CommodoreEntries::Unlisten, 0xf183, 0xf1b9},
        {"ACPTR", &CommodoreEntries::Acptr, 0xf18c, 0xf1c0},
        {"OPENI", &CommodoreEntries::Openi, 0xf466, 0xf4a5},
        {"CLSE1", &CommodoreEntries::Clse1, 0xf6f0, 0xf72f},
        {"CLOS5", &CommodoreEntries::Clos5, 0xf2ae, 0xf2e2},
    }};
    const auto upgrade = test_rom::metadata(Set::Upgrade);
    const auto rom4 = test_rom::metadata(Set::Rom4);
    EXPECT_EQ(upgrade.set, Set::Upgrade);
    EXPECT_EQ(rom4.set, Set::Rom4);
    EXPECT_STREQ(upgrade.identity, "PET upgrade ROM 2");
    EXPECT_STREQ(rom4.identity, "PET ROM 4");
    EXPECT_EQ(upgrade.cpu, CPU_SOFT_6502);
    EXPECT_EQ(rom4.cpu, CPU_SOFT_6502);
    EXPECT_EQ(upgrade.waterloo, nullptr);
    EXPECT_EQ(rom4.waterloo, nullptr);
    ASSERT_NE(upgrade.commodore, nullptr);
    ASSERT_NE(rom4.commodore, nullptr);
    for (const auto& entry : entries) {
        SCOPED_TRACE(entry.name);
        EXPECT_EQ(upgrade.commodore->*entry.member, CpuAddress{entry.upgrade});
        EXPECT_EQ(rom4.commodore->*entry.member, CpuAddress{entry.rom4});
    }
    EXPECT_FALSE(upgrade.commodore->Trans1);
    ASSERT_TRUE(rom4.commodore->Trans1);
    EXPECT_EQ(*rom4.commodore->Trans1, CpuAddress{0xda9b});
}

TEST(RomCatalog, WaterlooMetadataMatchesRevision12Ledger) {
    // Independent expectations from the archived revision-12 callable-entry ledger.
    struct Expected {
        const char* name;
        CpuAddress WaterlooEntries::* member;
        uint16_t address;
    };
    constexpr std::array<Expected, 21> entries{{
        {"IEEEInit", &WaterlooEntries::IeeeInit, 0xc188},
        {"ATNDown", &WaterlooEntries::AtnDown, 0xc14f},
        {"ATNUp", &WaterlooEntries::AtnUp, 0xc16e},
        {"EOIDown", &WaterlooEntries::EoiDown, 0xc176},
        {"EOIUp", &WaterlooEntries::EoiUp, 0xc17e},
        {"UNListen", &WaterlooEntries::Unlisten, 0xc0d0},
        {"UNTalk", &WaterlooEntries::Untalk, 0xc13c},
        {"SetLstnr", &WaterlooEntries::Listen, 0xc072},
        {"SetTalkr", &WaterlooEntries::Talk, 0xc0dd},
        {"EOI byte", &WaterlooEntries::EoiByte, 0xbe5f},
        {"final byte", &WaterlooEntries::FinalByte, 0xbe53},
        {"ByteOut", &WaterlooEntries::ByteOut, 0xbe6f},
        {"OPEN", &WaterlooEntries::Open, 0xbcd0},
        {"CLOSE", &WaterlooEntries::Close, 0xbd11},
        {"REL OPEN", &WaterlooEntries::RelOpen, 0xbd51},
        {"SCRATCH", &WaterlooEntries::Scratch, 0xbd96},
        {"RENAME", &WaterlooEntries::Rename, 0xbdc6},
        {"initialize", &WaterlooEntries::Initialize, 0xbe23},
        {"byte input", &WaterlooEntries::Input, 0xbefb},
        {"REL position", &WaterlooEntries::RelPosition, 0xbf98},
        {"status", &WaterlooEntries::Status, 0xbfef},
    }};
    const auto metadata = test_rom::metadata(Set::Waterloo);
    EXPECT_EQ(metadata.set, Set::Waterloo);
    EXPECT_STREQ(metadata.identity, "Waterloo revision 12");
    EXPECT_EQ(metadata.cpu, CPU_SOFT_6809);
    EXPECT_EQ(metadata.commodore, nullptr);
    ASSERT_NE(metadata.waterloo, nullptr);
    for (const auto& entry : entries) {
        SCOPED_TRACE(entry.name);
        EXPECT_EQ(metadata.waterloo->*entry.member, CpuAddress{entry.address});
    }
}

TEST(RomCatalog, DescriptorAndImageStorageRemainStableAcrossLookups) {
    for (const auto set : {Set::Upgrade, Set::Rom4, Set::Waterloo}) {
        const auto& descriptor = test_rom::descriptor(set);
        const auto images = test_rom::images(set);
        const auto metadata = test_rom::metadata(set);
        for (const auto other : {Set::Upgrade, Set::Rom4, Set::Waterloo})
            EXPECT_EQ(test_rom::descriptor(other).metadata.set, other);
        EXPECT_EQ(&test_rom::descriptor(set), &descriptor);
        EXPECT_EQ(test_rom::images(set).data(), images.data());
        EXPECT_EQ(images.data(), descriptor.images.data());
        EXPECT_EQ(metadata.set, descriptor.metadata.set);
        EXPECT_EQ(metadata.identity, descriptor.metadata.identity);
        EXPECT_EQ(metadata.cpu, descriptor.metadata.cpu);
        EXPECT_EQ(metadata.commodore, descriptor.metadata.commodore);
        EXPECT_EQ(metadata.waterloo, descriptor.metadata.waterloo);
    }
}

TEST(RomCatalog, UnknownSetsRejectEveryCatalogAccessor) {
    for (const auto set : {static_cast<Set>(-1), static_cast<Set>(3), static_cast<Set>(99)}) {
        EXPECT_THROW(test_rom::descriptor(set), std::invalid_argument);
        EXPECT_THROW(test_rom::metadata(set), std::invalid_argument);
        EXPECT_THROW(test_rom::images(set), std::invalid_argument);
    }
}
