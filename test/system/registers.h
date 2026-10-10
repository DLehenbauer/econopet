// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "io.h"
#include "types.h"

// Host-visible addresses mirror common_pkg.sv and the production SPI driver.
namespace fpga {
enum class Register : uint32_t {
    Status = ECONOPET_WB_STATUS_ADDR,
    CpuControl = ECONOPET_WB_CPU_ADDR,
    Video = ECONOPET_WB_VIDEO_ADDR,
    Breakpoint = ECONOPET_WB_BP_CTL_ADDR,
    BreakpointAddressHigh = ECONOPET_WB_BP_HI_ADDR,
    CpuSelect = ECONOPET_WB_CPU_SEL_ADDR,
    IeeeControl = ECONOPET_WB_IEEE_CTRL_ADDR,
    IeeeStatus = ECONOPET_WB_IEEE_STATUS_ADDR,
    IeeeRx = ECONOPET_WB_IEEE_RX_ADDR,
    IeeeTx = ECONOPET_WB_IEEE_TX_ADDR,
    IeeeTxLast = ECONOPET_WB_IEEE_TX_LAST_ADDR,
    IeeeSecondaryAddress = ECONOPET_WB_IEEE_SA_ADDR,
    IeeeTxStatus = ECONOPET_WB_IEEE_TXS_ADDR,
    IeeeTxStatusLast = ECONOPET_WB_IEEE_TXS_LAST_ADDR,
};
enum class StatusBit : uint8_t {
    Graphics = ECONOPET_REG_STATUS_GRAPHICS_MASK,
    FixedDisplay = ECONOPET_REG_STATUS_CRT_MASK,
    GraphicsKeyboard = ECONOPET_REG_STATUS_KEYBOARD_MASK,
    BreakpointHalt = ECONOPET_REG_STATUS_BP_HALT_MASK,
    PhysicalCpu = ECONOPET_REG_STATUS_PHYS_CPU_MASK,
    All = Graphics | FixedDisplay | GraphicsKeyboard | BreakpointHalt | PhysicalCpu,
};
using Status = econopet::Flags<StatusBit>;
using econopet::operator|;

enum class SpiCommand : uint8_t {
    ReadAt = ECONOPET_SPI_CMD_READ_AT,
    ReadNext = ECONOPET_SPI_CMD_READ_NEXT,
    ReadPrev = ECONOPET_SPI_CMD_READ_PREV,
    ReadSame = ECONOPET_SPI_CMD_READ_SAME,
    WriteAt = ECONOPET_SPI_CMD_WRITE_AT,
    WriteNext = ECONOPET_SPI_CMD_WRITE_NEXT,
    WritePrev = ECONOPET_SPI_CMD_WRITE_PREV,
    WriteSame = ECONOPET_SPI_CMD_WRITE_SAME,
};

// Map physical SRAM into the direct FPGA window (common_pkg::wb_ram_addr).
constexpr econopet::WishboneAddress address(econopet::SramAddress location) {
    return econopet::WishboneAddress{location.value()};
}

// Validate a logical register before returning its checked Wishbone address.
constexpr econopet::WishboneAddress address(Register reg) {
    switch (reg) {
    case Register::Status:
    case Register::CpuControl:
    case Register::Video:
    case Register::Breakpoint:
    case Register::BreakpointAddressHigh:
    case Register::CpuSelect:
    case Register::IeeeControl:
    case Register::IeeeStatus:
    case Register::IeeeRx:
    case Register::IeeeTx:
    case Register::IeeeTxLast:
    case Register::IeeeSecondaryAddress:
    case Register::IeeeTxStatus:
    case Register::IeeeTxStatusLast:
        return econopet::WishboneAddress{std::to_underlying(reg)};
    }
    throw std::invalid_argument("unsupported FPGA register");
}

// Encode a supported command with checked absolute-address high bits.
constexpr uint8_t command_byte(SpiCommand command, econopet::WishboneAddress address = {}) {
    switch (command) {
    case SpiCommand::ReadAt:
    case SpiCommand::WriteAt:
        return static_cast<uint8_t>(std::to_underlying(command) | (address.value() >> 16));
    case SpiCommand::ReadNext:
    case SpiCommand::ReadPrev:
    case SpiCommand::ReadSame:
    case SpiCommand::WriteNext:
    case SpiCommand::WritePrev:
    case SpiCommand::WriteSame:
        if (address.value() != 0)
            throw std::invalid_argument("relative SPI command cannot contain an address");
        return std::to_underlying(command);
    }
    throw std::invalid_argument("unsupported SPI command");
}

// Require exactly one supported command before the transport can mutate the board.
inline void validate_command(econopet::ByteView bytes) {
    constexpr uint8_t OpcodeMask = ECONOPET_SPI_CMD_OPCODE_MASK;
    constexpr uint8_t ReservedMask = ECONOPET_SPI_CMD_RESERVED_MASK;
    constexpr uint8_t AddressHighMask = ECONOPET_SPI_CMD_ADDRESS_HIGH_MASK;
    constexpr size_t AbsoluteReadSize = 3;
    constexpr size_t RelativeReadSize = 1;
    if (bytes.size() == 0) throw std::invalid_argument("empty FPGA SPI command");
    const auto command = static_cast<SpiCommand>(bytes[0] & OpcodeMask);
    const bool absolute = command == SpiCommand::ReadAt || command == SpiCommand::WriteAt;
    const bool write = (bytes[0] & std::to_underlying(SpiCommand::WriteSame)) != 0;
    if ((bytes[0] & ReservedMask) || (!absolute && (bytes[0] & AddressHighMask)))
        throw std::invalid_argument("FPGA SPI command contains reserved bits");
    const size_t expected = (absolute ? AbsoluteReadSize : RelativeReadSize) + (write ? 1 : 0);
    if (bytes.size() != expected)
        throw std::invalid_argument("FPGA SPI command has an invalid byte count");
}
} // namespace fpga

// PET physical I/O addresses combine board decoding with chip register indices.
namespace pet {
// Map a checked VIA register into the PET CPU address space.
constexpr econopet::CpuAddress address(econopet::io::ViaRegister reg) {
    if (std::to_underlying(reg) > M6522_REG_RA_NOH)
        throw std::out_of_range("invalid PET VIA register");
    return econopet::CpuAddress{0xe840 + std::to_underlying(reg)};
}
inline constexpr econopet::CpuAddress Pia1PortA{0xe810 + M6520_REG_RA};
inline constexpr econopet::CpuAddress Pia1ControlA{0xe810 + M6520_REG_CRA};
inline constexpr econopet::CpuAddress Pia1PortB{0xe810 + M6520_REG_RB};
inline constexpr econopet::CpuAddress Pia1ControlB{0xe810 + M6520_REG_CRB};
inline constexpr econopet::CpuAddress Pia2PortA{0xe820 + M6520_REG_RA};
inline constexpr econopet::CpuAddress Pia2ControlA{0xe820 + M6520_REG_CRA};
inline constexpr econopet::CpuAddress Pia2PortB{0xe820 + M6520_REG_RB};
inline constexpr econopet::CpuAddress Pia2ControlB{0xe820 + M6520_REG_CRB};
inline constexpr econopet::CpuAddress ViaPortB{0xe840 + M6522_REG_RB};
inline constexpr econopet::CpuAddress ViaPortA{0xe840 + M6522_REG_RA};
inline constexpr econopet::CpuAddress ViaDdrB{0xe840 + M6522_REG_DDRB};
inline constexpr econopet::CpuAddress ViaDdrA{0xe840 + M6522_REG_DDRA};
inline constexpr econopet::CpuAddress ViaTimer1Low{0xe840 + M6522_REG_T1CL};
inline constexpr econopet::CpuAddress ViaTimer1High{0xe840 + M6522_REG_T1CH};
inline constexpr econopet::CpuAddress ViaTimer2Low{0xe840 + M6522_REG_T2CL};
inline constexpr econopet::CpuAddress ViaTimer2High{0xe840 + M6522_REG_T2CH};
inline constexpr econopet::CpuAddress ViaShift{0xe840 + M6522_REG_SR};
inline constexpr econopet::CpuAddress ViaAcr{0xe840 + M6522_REG_ACR};
inline constexpr econopet::CpuAddress ViaPcr{0xe840 + M6522_REG_PCR};
inline constexpr econopet::CpuAddress ViaIfr{0xe840 + M6522_REG_IFR};
inline constexpr econopet::CpuAddress ViaIer{0xe840 + M6522_REG_IER};
inline constexpr econopet::CpuAddress Reset6502{0xfffc};
inline constexpr econopet::CpuAddress Irq6502{0xfffe};
inline constexpr econopet::CpuAddress Reset6809{0xfffe};
} // namespace pet
