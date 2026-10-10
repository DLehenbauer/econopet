// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "registers.h"

#include <array>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace econopet;
constexpr WishboneAddress LastWishboneAddress{WishboneAddressCapacity - 1};
struct ProtocolCommand {
    fpga::SpiCommand command;
    uint8_t first_byte;
    uint8_t last_byte;
    size_t byte_count;
};
// Specify wire bytes and sizes independently of the production masks and validator.
constexpr std::array Protocol{
    ProtocolCommand{fpga::SpiCommand::ReadSame, 0x00, 0x00, 1},
    ProtocolCommand{fpga::SpiCommand::ReadNext, 0x20, 0x20, 1},
    ProtocolCommand{fpga::SpiCommand::ReadAt, 0x40, 0x4f, 3},
    ProtocolCommand{fpga::SpiCommand::ReadPrev, 0x60, 0x60, 1},
    ProtocolCommand{fpga::SpiCommand::WriteSame, 0x80, 0x80, 2},
    ProtocolCommand{fpga::SpiCommand::WriteNext, 0xa0, 0xa0, 2},
    ProtocolCommand{fpga::SpiCommand::WriteAt, 0xc0, 0xcf, 4},
    ProtocolCommand{fpga::SpiCommand::WritePrev, 0xe0, 0xe0, 2},
};
static_assert(!std::is_convertible_v<uint32_t, fpga::Register>);
static_assert(!std::is_constructible_v<CpuControl, fpga::StatusBit>);
}

TEST(BoardRegisters, SpiCommandShapesAreChecked) {
    for (const auto& command : Protocol) {
        EXPECT_EQ(fpga::command_byte(command.command), command.first_byte);
        std::vector<uint8_t> bytes(command.byte_count, 0x5a);
        bytes[0] = command.first_byte;
        EXPECT_NO_THROW(fpga::validate_command(bytes));
        bytes.push_back(0);
        EXPECT_THROW(fpga::validate_command(bytes), std::invalid_argument);
    }
}

TEST(BoardRegisters, AddressTranslationsPreserveTheirMemorySpace) {
    constexpr CpuAddress last_cpu{CpuAddressCapacity - 1};
    constexpr SramAddress backing{last_cpu.value()};
    static_assert(fpga::address(backing).value() == last_cpu.value());
    constexpr SramAddress last_sram{SramCapacity - 1};
    EXPECT_EQ(fpga::address(last_sram).value(), last_sram.value());
    EXPECT_EQ(fpga::command_byte(fpga::SpiCommand::ReadAt, fpga::address(last_sram)), 0x41);
    EXPECT_EQ(fpga::command_byte(fpga::SpiCommand::WriteAt, LastWishboneAddress), 0xcf);
    EXPECT_THROW(fpga::command_byte(fpga::SpiCommand::ReadAt, WishboneAddress{WishboneAddressCapacity}),
        std::out_of_range);
}

TEST(BoardRegisters, RegisterAndCommandEncodingsAreChecked) {
    EXPECT_THROW(fpga::Status::from_bits(0x80), std::invalid_argument);
    EXPECT_EQ(fpga::address(fpga::Register::CpuSelect).value(), ECONOPET_WB_CPU_SEL_ADDR);
    EXPECT_EQ(fpga::command_byte(fpga::SpiCommand::ReadAt, LastWishboneAddress), 0x4f);
    EXPECT_EQ(fpga::command_byte(fpga::SpiCommand::WriteAt), 0xc0);
    EXPECT_EQ(fpga::command_byte(fpga::SpiCommand::ReadNext), 0x20);
    EXPECT_THROW(fpga::address(static_cast<fpga::Register>(0)), std::invalid_argument);
    EXPECT_THROW(fpga::command_byte(static_cast<fpga::SpiCommand>(0xff)), std::invalid_argument);
    EXPECT_THROW(fpga::command_byte(fpga::SpiCommand::ReadNext, WishboneAddress{1}), std::invalid_argument);
    EXPECT_EQ(pet::Pia1ControlA.value(), 0xe811u);
    EXPECT_EQ(pet::Pia2PortB.value(), 0xe822u);
    EXPECT_EQ(pet::address(io::ViaRegister::Ier), pet::ViaIer);
    EXPECT_THROW(pet::address(static_cast<io::ViaRegister>(0xff)), std::out_of_range);
}

TEST(BoardRegisters, AllCommandBytesRejectReservedBitsAndWrongLengths) {
    for (unsigned value = 0; value <= UINT8_MAX; ++value) {
        const auto byte = static_cast<uint8_t>(value);
        for (const size_t length : {0u, 1u, 2u, 3u, 4u, 5u}) {
            bool valid = false;
            for (const auto& entry : Protocol)
                if (byte >= entry.first_byte && byte <= entry.last_byte && length == entry.byte_count)
                    valid = true;
            std::vector<uint8_t> command(length, 0x5a);
            if (!command.empty()) command[0] = byte;
            if (valid) {
                EXPECT_NO_THROW(fpga::validate_command(command)) << value << " length=" << length;
            } else {
                EXPECT_THROW(fpga::validate_command(command), std::invalid_argument) << value << " length=" << length;
            }
        }
    }
}
