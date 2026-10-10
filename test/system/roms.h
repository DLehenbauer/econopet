// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>

extern "C" {
#include "driver.h"
}

#include "types.h"

namespace test_rom {
enum class Set { Upgrade, Rom4, Waterloo };

// Revision-specific entries from .github/instructions/ieee.instructions.md's PET ledger.
struct CommodoreEntries {
    econopet::CpuAddress Cint, Talk, Listen, List1, Isour, Secondary, Scatn;
    econopet::CpuAddress Er001, Tksa, Tkatn;
    econopet::CpuAddress Ciout;
    econopet::CpuAddress Untalk, Unlisten, Acptr;
    econopet::CpuAddress Openi, Clse1, Clos5;
    std::optional<econopet::CpuAddress> Trans1;
};
inline constexpr CommodoreEntries UpgradeEntries{
    .Cint = econopet::CpuAddress{0xe1de},
    .Talk = econopet::CpuAddress{0xf0b6},
    .Listen = econopet::CpuAddress{0xf0ba},
    .List1 = econopet::CpuAddress{0xf0bc},
    .Isour = econopet::CpuAddress{0xf0ee},
    .Secondary = econopet::CpuAddress{0xf128},
    .Scatn = econopet::CpuAddress{0xf12d},
    .Er001 = econopet::CpuAddress{0xf146},
    .Tksa = econopet::CpuAddress{0xf164},
    .Tkatn = econopet::CpuAddress{0xf169},
    .Ciout = econopet::CpuAddress{0xf16f},
    .Untalk = econopet::CpuAddress{0xf17f},
    .Unlisten = econopet::CpuAddress{0xf183},
    .Acptr = econopet::CpuAddress{0xf18c},
    .Openi = econopet::CpuAddress{0xf466},
    .Clse1 = econopet::CpuAddress{0xf6f0},
    .Clos5 = econopet::CpuAddress{0xf2ae},
    .Trans1 = std::nullopt,
};
inline constexpr CommodoreEntries Rom4Entries{
    .Cint = econopet::CpuAddress{0xe000},
    .Talk = econopet::CpuAddress{0xf0d2},
    .Listen = econopet::CpuAddress{0xf0d5},
    .List1 = econopet::CpuAddress{0xf0d7},
    .Isour = econopet::CpuAddress{0xf109},
    .Secondary = econopet::CpuAddress{0xf143},
    .Scatn = econopet::CpuAddress{0xf148},
    .Er001 = econopet::CpuAddress{0xf175},
    .Tksa = econopet::CpuAddress{0xf193},
    .Tkatn = econopet::CpuAddress{0xf198},
    .Ciout = econopet::CpuAddress{0xf19e},
    .Untalk = econopet::CpuAddress{0xf1ae},
    .Unlisten = econopet::CpuAddress{0xf1b9},
    .Acptr = econopet::CpuAddress{0xf1c0},
    .Openi = econopet::CpuAddress{0xf4a5},
    .Clse1 = econopet::CpuAddress{0xf72f},
    .Clos5 = econopet::CpuAddress{0xf2e2},
    .Trans1 = econopet::CpuAddress{0xda9b},
};

// Exact Waterloo revision-12 entries, including prepared-state boundaries.
struct WaterlooEntries {
    econopet::CpuAddress IeeeInit, AtnDown, AtnUp, EoiDown, EoiUp;
    econopet::CpuAddress Unlisten, Untalk, Listen, Talk, EoiByte, FinalByte, ByteOut;
    econopet::CpuAddress Open, Close, RelOpen, Scratch, Rename, Initialize;
    econopet::CpuAddress Input, RelPosition, Status;
};
inline constexpr WaterlooEntries WaterlooRevision12{
    .IeeeInit = econopet::CpuAddress{0xc188},
    .AtnDown = econopet::CpuAddress{0xc14f},
    .AtnUp = econopet::CpuAddress{0xc16e},
    .EoiDown = econopet::CpuAddress{0xc176},
    .EoiUp = econopet::CpuAddress{0xc17e},
    .Unlisten = econopet::CpuAddress{0xc0d0},
    .Untalk = econopet::CpuAddress{0xc13c},
    .Listen = econopet::CpuAddress{0xc072},
    .Talk = econopet::CpuAddress{0xc0dd},
    .EoiByte = econopet::CpuAddress{0xbe5f},
    .FinalByte = econopet::CpuAddress{0xbe53},
    .ByteOut = econopet::CpuAddress{0xbe6f},
    .Open = econopet::CpuAddress{0xbcd0},
    .Close = econopet::CpuAddress{0xbd11},
    .RelOpen = econopet::CpuAddress{0xbd51},
    .Scratch = econopet::CpuAddress{0xbd96},
    .Rename = econopet::CpuAddress{0xbdc6},
    .Initialize = econopet::CpuAddress{0xbe23},
    .Input = econopet::CpuAddress{0xbefb},
    .RelPosition = econopet::CpuAddress{0xbf98},
    .Status = econopet::CpuAddress{0xbfef},
};

struct Metadata {
    Set set;
    const char* identity;
    cpu_type_t cpu;
    const CommodoreEntries* commodore;
    const WaterlooEntries* waterloo;
};

struct Image {
    const char* filename;
    econopet::SramAddress address;
    size_t size;
};

inline constexpr std::array<Image, 4> UpgradeImages{{
    {"basic-2-c000.901465-01.bin", econopet::SramAddress{0xc000}, 4096},
    {"basic-2-d000.901465-02.bin", econopet::SramAddress{0xd000}, 4096},
    {"edit-2-n.901447-24.bin", econopet::SramAddress{0xe000}, 2048},
    {"kernal-2.901465-03.bin", econopet::SramAddress{0xf000}, 4096},
}};
inline constexpr std::array<Image, 5> Rom4Images{{
    {"basic-4-b000.901465-23.bin", econopet::SramAddress{0xb000}, 4096},
    {"basic-4-c000.901465-20.bin", econopet::SramAddress{0xc000}, 4096},
    {"basic-4-d000.901465-21.bin", econopet::SramAddress{0xd000}, 4096},
    {"edit-4-n.901447-29.bin", econopet::SramAddress{0xe000}, 2048},
    {"kernal-4.901465-22.bin", econopet::SramAddress{0xf000}, 4096},
}};
inline constexpr std::array<Image, 3> WaterlooImages{{
    {"waterloo-a000-bfff.970018-12.bin", econopet::SramAddress{0xa000}, 8192},
    {"waterloo-c000-dfff.970019-12.bin", econopet::SramAddress{0xc000}, 8192},
    {"waterloo-e000-ffff-970034-12.bin", econopet::SramAddress{0xe000}, 8192},
}};

// Bind revision identity and immutable physical mappings in one supported-set catalog.
struct Descriptor {
    Metadata metadata;
    std::span<const Image> images;
};
inline constexpr Descriptor Upgrade{
    .metadata = {.set = Set::Upgrade, .identity = "PET upgrade ROM 2", .cpu = CPU_SOFT_6502,
                 .commodore = &UpgradeEntries, .waterloo = nullptr},
    .images = UpgradeImages,
};
inline constexpr Descriptor Rom4{
    .metadata = {.set = Set::Rom4, .identity = "PET ROM 4", .cpu = CPU_SOFT_6502,
                 .commodore = &Rom4Entries, .waterloo = nullptr},
    .images = Rom4Images,
};
inline constexpr Descriptor Waterloo{
    .metadata = {.set = Set::Waterloo, .identity = "Waterloo revision 12", .cpu = CPU_SOFT_6809,
                 .commodore = nullptr, .waterloo = &WaterlooRevision12},
    .images = WaterlooImages,
};

// Resolve a supported revision without allocating or accepting unknown set values.
inline const Descriptor& descriptor(Set set) {
    switch (set) {
    case Set::Upgrade:
        return Upgrade;
    case Set::Rom4:
        return Rom4;
    case Set::Waterloo:
        return Waterloo;
    }
    throw std::invalid_argument("unsupported ROM set");
}

// Describe the selected base revision, independent of paths and deliberate patches.
inline Metadata metadata(Set set) { return descriptor(set).metadata; }

// Borrow the static mappings of a complete set without allocating a container.
inline std::span<const Image> images(Set set) { return descriptor(set).images; }
} // namespace test_rom
