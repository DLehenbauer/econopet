// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <utility>

#include "firmware_transport.h"

extern "C" {
#include "driver.h"
#include "global.h"
}

static_assert(std::to_underlying(econopet::CpuControlBit::Ready) == CPU_READY);
static_assert(std::to_underlying(econopet::CpuControlBit::Reset) == CPU_RESET);
static_assert(std::to_underlying(econopet::CpuControlBit::Nmi) == CPU_NMI);

namespace {
// Check the full transfer before any SPI operation or caller buffer mutation.
econopet::WishboneAddress checked_range(uint32_t address, size_t length) {
    const econopet::WishboneAddress start{address};
    if (length > econopet::WishboneAddressCapacity - start.value())
        throw std::out_of_range("firmware SPI transfer exceeds the Wishbone address space");
    firmware_system();
    return start;
}

// Perform one command under normal CS ownership and return the first RX byte.
uint8_t command(econopet::ByteView bytes) {
    firmware_take_read_status();
    return firmware_system().spi().transaction([&](auto& transaction) {
        return transaction.command(bytes);
    });
}

// Encode an addressed write without truncating the validated 20-bit address.
std::array<uint8_t, 4> write_at(econopet::WishboneAddress address, uint8_t value) {
    return {fpga::command_byte(fpga::SpiCommand::WriteAt, address),
        static_cast<uint8_t>(address.value() >> 8),
        static_cast<uint8_t>(address.value()), value};
}

// Perform one pipelined relative read using the FPGA's retained address pointer.
uint8_t read_relative(fpga::SpiCommand op) {
    const uint8_t bytes[] = {fpga::command_byte(op)};
    return command(bytes);
}

// Perform one relative write and return the previously queued read result.
uint8_t write_relative(fpga::SpiCommand op, uint8_t value) {
    const uint8_t bytes[] = {fpga::command_byte(op), value};
    return command(bytes);
}
} // namespace

// Queue a production READ_AT, without fetching its pipelined result.
extern "C" void spi_read_seek(uint32_t address) {
    const auto start = checked_range(address, 1);
    const uint8_t bytes[] = {fpga::command_byte(fpga::SpiCommand::ReadAt, start),
        static_cast<uint8_t>(start.value() >> 8), static_cast<uint8_t>(start.value())};
    command(bytes);
}

// Return the prior read and queue the next address, including RTL wraparound.
extern "C" uint8_t spi_read_next() { return read_relative(fpga::SpiCommand::ReadNext); }
// Return the prior read and queue the preceding address.
extern "C" uint8_t spi_read_prev() { return read_relative(fpga::SpiCommand::ReadPrev); }
// Return the prior read and queue the same address.
extern "C" uint8_t spi_read_same() { return read_relative(fpga::SpiCommand::ReadSame); }

// Match the production driver's two-transaction absolute read.
extern "C" uint8_t spi_read_at(uint32_t address) {
    checked_range(address, 1);
    const auto previous_status = firmware_take_read_status();
    spi_read_seek(address);
    const auto value = spi_read_next();
    firmware_record_read(address, value, previous_status);
    return value;
}

// Match production READ_AT followed by N separate READ_NEXT transactions.
// A zero-length read still seeks, as in driver.c.
extern "C" void spi_read(uint32_t address, size_t length, uint8_t* destination) {
    checked_range(address, length);
    if (length && !destination) throw std::invalid_argument("null firmware SPI read destination");
    spi_read_seek(address);
    for (size_t index = 0; index < length; ++index) destination[index] = spi_read_next();
}

// Write one absolute address and return the first RX byte, not the final byte.
extern "C" uint8_t spi_write_at(uint32_t address, uint8_t value) {
    const auto bytes = write_at(checked_range(address, 1), value);
    return command(bytes);
}

// Write the next address using the FPGA pointer and pipeline.
extern "C" uint8_t spi_write_next(uint8_t value) {
    return write_relative(fpga::SpiCommand::WriteNext, value);
}
// Write the preceding address using the FPGA pointer and pipeline.
extern "C" uint8_t spi_write_prev(uint8_t value) {
    return write_relative(fpga::SpiCommand::WritePrev, value);
}
// Write the same address using the FPGA pointer and pipeline.
extern "C" uint8_t spi_write_same(uint8_t value) {
    return write_relative(fpga::SpiCommand::WriteSame, value);
}

// Match production WRITE_AT followed by separate WRITE_NEXT transactions.
extern "C" void spi_write(uint32_t address, const uint8_t* source, size_t length) {
    checked_range(address, length);
    if (length && !source) throw std::invalid_argument("null firmware SPI write source");
    if (!length) return;
    spi_write_at(address, source[0]);
    for (size_t index = 1; index < length; ++index) spi_write_next(source[index]);
}

// Match the production FIFO burst: one CS, WRITE_AT, then stalled WRITE_SAMEs.
extern "C" void spi_write_same_block(uint32_t address, const uint8_t* source, size_t length) {
    const auto start = checked_range(address, 1);
    if (length && !source) throw std::invalid_argument("null firmware SPI burst source");
    if (!length) return;
    firmware_take_read_status();
    const auto first = write_at(start, source[0]);
    firmware_system().spi().transaction([&](auto& transaction) {
        transaction.command(first);
        for (size_t index = 1; index < length; ++index) {
            const uint8_t bytes[] = {fpga::command_byte(fpga::SpiCommand::WriteSame), source[index]};
            transaction.command(bytes);
        }
    });
}

// Preserve production fill chunk boundaries without using its global allocator.
extern "C" void spi_fill(uint32_t address, uint8_t value, size_t length) {
    checked_range(address, length);
    std::array<uint8_t, TEMP_BUFFER_SIZE> bytes;
    bytes.fill(value);
    while (length) {
        const auto chunk = std::min(length, bytes.size());
        spi_write(address, bytes.data(), chunk);
        address += chunk;
        length -= chunk;
    }
}
