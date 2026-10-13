// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include <type_traits>

#include <gmock/gmock.h>

#include "registers.h"
#include "transactions.h"

namespace {
using namespace ieee_test;
using testing::ElementsAre;
using testing::HasSubstr;

static_assert(!std::is_convertible_v<unsigned, Device>);
static_assert(!std::is_convertible_v<unsigned, Channel>);
static_assert(!std::is_convertible_v<Channel, Device>);
static_assert(!std::is_constructible_v<Command, CommandCategory, uint64_t>);
static_assert(!std::is_constructible_v<Command, CommandCategory, uint8_t>);
static_assert(!std::is_constructible_v<Command, CommandCategory>);
static_assert(!std::is_invocable_v<decltype(Listen), Channel>);
static_assert(!std::is_invocable_v<decltype(Open), Device>);
static_assert(!std::is_constructible_v<fpga::IeeeControl, fpga::IeeeStatusBit>);
static_assert(!std::is_constructible_v<fpga::IeeeControl, fpga::IeeeControlStatusBit>);

// Readback and command masks remain distinct, and status bit 7 is never EOI.
TEST(IeeeTransactions, RegisterFlagsPreserveTheirHardwareDomains) {
    EXPECT_EQ(pet::EoiInput, 0x40);
    EXPECT_EQ((fpga::IeeeControlBit::Enabled | fpga::IeeeControlBit::Flush).bits(), 0x03);
    EXPECT_EQ(fpga::IeeeControlStatus{fpga::IeeeControlStatusBit::TxRoom}.bits(), 0x02);
    EXPECT_TRUE(fpga::IeeeStatus::from_bits(0x80).contains(fpga::IeeeStatusBit::TalkStarved));
    EXPECT_FALSE(fpga::IeeeStatus::from_bits(0x80).contains(fpga::IeeeStatusBit::RxReady));
    EXPECT_THROW(fpga::IeeeControl::from_bits(0x08), std::invalid_argument);
    EXPECT_THROW(fpga::IeeeControlStatus::from_bits(0x04), std::invalid_argument);
}

// Check encodings against literal protocol bytes rather than production macros.
TEST(IeeeTransactions, CheckedCommandDomainsAndEncodings) {
    EXPECT_EQ(Listen(Device{0}).byte(), 0x20);
    EXPECT_EQ(Listen(Device{30}).byte(), 0x3e);
    EXPECT_EQ(Talk(Device{8}).byte(), 0x48);
    EXPECT_EQ(Talk(Device{30}).byte(), 0x5e);
    EXPECT_EQ(Secondary(ieee_test::SecondaryAddress{31}).byte(), 0x7f);
    EXPECT_EQ(Secondary(Channel{15}).byte(), 0x6f);
    EXPECT_EQ(Open(Channel{0}).byte(), 0xf0);
    EXPECT_EQ(Open(Channel{15}).byte(), 0xff);
    EXPECT_EQ(Close(Channel{15}).byte(), 0xef);
    EXPECT_EQ(Unlisten.byte(), 0x3f);
    EXPECT_EQ(Untalk.byte(), 0x5f);
    EXPECT_EQ(Listen(Device{8}).category(), CommandCategory::Listen);
    EXPECT_EQ(Talk(Device{8}).category(), CommandCategory::Talk);
    EXPECT_EQ(Secondary(Channel{0}).category(), CommandCategory::Secondary);
    EXPECT_EQ(Open(Channel{0}).category(), CommandCategory::Open);
    EXPECT_EQ(Close(Channel{0}).category(), CommandCategory::Close);
    EXPECT_EQ(Unlisten.category(), CommandCategory::Unlisten);
    EXPECT_EQ(Untalk.category(), CommandCategory::Untalk);
    EXPECT_THROW(Device{31}, std::out_of_range);
    EXPECT_THROW(Device{256}, std::out_of_range);
    EXPECT_THROW(Device{uint64_t{1} << 32}, std::out_of_range);
    EXPECT_THROW(Device(-1), std::out_of_range);
    EXPECT_THROW(Channel{16}, std::out_of_range);
    EXPECT_THROW(Channel{uint64_t{1} << 32}, std::out_of_range);
    EXPECT_THROW(ieee_test::SecondaryAddress{32}, std::out_of_range);
    EXPECT_THROW(Open(Channel{16}), std::out_of_range);
    EXPECT_THROW(Listen(Device{31}), std::out_of_range);
}

// Named records compose directly with standard exact sequence matchers.
TEST(IeeeTransactions, ExpectedRecordsComposeWithElementsAre) {
    const std::vector<Record> records{
        command(Listen(Device{8})), data('A'), data('B', Eoi::Last), command(Unlisten)};
    EXPECT_THAT(records, ElementsAre(command(Listen(Device{8})), data('A'),
        data('B', Eoi::Last), command(Unlisten)));
    EXPECT_NO_THROW(require_sequence(records, records, "captured"));
    EXPECT_NO_THROW(require_sequence({}, {}, "no records"));
    EXPECT_THROW(data(0, Eoi::Unobserved), std::invalid_argument);
    EXPECT_THROW(data(0, static_cast<Eoi>(99)), std::invalid_argument);
}

// Every differing field, missing record and extra record names the exact index.
TEST(IeeeTransactions, SequenceDiagnosticsIncludeByteAtnAndEoi) {
    const std::vector<Record> expected{command(Unlisten), data('B', Eoi::Last)};
    for (const auto bad : {data('C', Eoi::Last), raw_record('B', true, Eoi::Last), data('B')}) {
        const auto message = sequence_mismatch({command(Unlisten), bad}, expected);
        ASSERT_TRUE(message);
        EXPECT_THAT(*message, HasSubstr("index=1"));
        EXPECT_THAT(*message, HasSubstr("byte=0x42 ATN=0 EOI=1"));
        EXPECT_THAT(*message, HasSubstr(describe(bad)));
    }
    EXPECT_THAT(*sequence_mismatch({command(Unlisten)}, expected), HasSubstr("actual {missing}"));
    EXPECT_THAT(*sequence_mismatch(expected, {command(Unlisten)}),
        HasSubstr("index=1: expected {end}, actual {byte=0x42 ATN=0 EOI=1}"));
    EXPECT_THROW(require_sequence({data('A')}, expected, "mismatch"), std::runtime_error);
    EXPECT_THAT(describe(fifo_record(data('B'))), HasSubstr("EOI=unobserved"));
    EXPECT_THAT(describe(Record{'B', false, static_cast<Eoi>(99)}), HasSubstr("EOI=invalid(99)"));
    EXPECT_TRUE(sequence_mismatch({fifo_record(data('B'))}, {data('B')}));
}

} // namespace
