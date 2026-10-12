// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include "fixture.h"
#include "system.h"
#include "transactions.h"

// RAM mailbox program acts as a PET controller. Every I/O transaction executes
// on the production soft 6502 through top.sv decoding, never RTL backdoors.
// Reserves mailbox, code, stack and vectors. Construct before enabling the drive.
class IeeeController {
public:
    // Installs the mailbox loop and selects PIA1's external EOI input.
    explicit IeeeController(System& system) : system_(system) {
        test_asm::Scenario scenario(system_);
        scenario.reserve("mailbox", test_asm::RegionKind::Mailbox, CommandMailbox, 3);
        scenario.reserve("stack", test_asm::RegionKind::Stack, econopet::CpuAddress{0x0100}, 0x100);
        scenario.reserve("vectors", test_asm::RegionKind::Vectors, econopet::CpuAddress{0xfffa}, 6);
        const auto code = scenario.reserve("controller", test_asm::RegionKind::Code, ieee_test::HarnessStart, 0x100);
        test_asm::mos6502::Program program(scenario, code);
        const auto poll = program.mark("poll");
        program.emit(test_asm::mos6502::lda_abs(CommandMailbox));
        program.emit(test_asm::mos6502::beq(poll));
        program.emit(test_asm::mos6502::lda_abs(WriteMailbox));
        const auto transfer = program.mark("transfer");
        program.emit(test_asm::mos6502::sta_abs(pet::Pia1PortA));
        program.emit(test_asm::mos6502::sta_abs(ReadMailbox));
        program.emit(test_asm::mos6502::lda_imm(MailboxIdle));
        program.emit(test_asm::mos6502::sta_abs(CommandMailbox));
        program.emit(test_asm::mos6502::jmp_abs(poll));
        transfer_opcode_ = program.address(transfer);
        program.install();
        system_.poke(econopet::SramAddress{CommandMailbox.value()}, MailboxIdle);
        system_.cpu(CPU_SOFT_6502).start(ieee_test::HarnessStart);
        system_.tick(StartupBudget);
        write(pet::Pia1ControlA, ieee_test::PiaC2High.bits());
    }
    IeeeController(const IeeeController&) = delete;
    IeeeController& operator=(const IeeeController&) = delete;
    IeeeController(IeeeController&&) = delete;
    IeeeController& operator=(IeeeController&&) = delete;

    // Interrupted mailbox or handshake work permanently invalidates this controller.
    bool peek_failed() const { return failed_; }
    // Synchronously executes one CPU write while preserving IEEE role state.
    void write(econopet::CpuAddress address, uint8_t value) { transfer(address, value, Access::Write); }
    // Synchronously executes one CPU read, including production I/O injection.
    uint8_t read(econopet::CpuAddress address) { return transfer(address, 0, Access::Read); }
    // Send a checked IEEE command with ATN asserted and EOI released.
    void command(ieee_test::Command value) { raw_send(ieee_test::command(value)); }
    // Send payload with ATN released and an explicit final-byte EOI policy.
    void data(uint8_t value, ieee_test::Eoi eoi = ieee_test::Eoi::More) {
        raw_send(ieee_test::data(value, eoi));
    }
    // Deliberate wire-level escape for unusual command bytes and flag combinations.
    void raw_send(ieee_test::Record record) {
        record = ieee_test::raw_record(record.data, record.atn, record.eoi);
        // Present the byte with DAV released and the requested ATN polarity.
        write(pet::ViaPortB, record.atn ? AtnAsserted : ieee_test::BusReleased);
        write(pet::Pia1ControlA, (record.eoi == ieee_test::Eoi::Last
            ? ieee_test::PiaC2Low : ieee_test::PiaC2High).bits());
        write(pet::Pia2ControlA, ieee_test::PiaC2High.bits());
        write(pet::Pia2ControlB, ieee_test::PiaC2High.bits());
        write(pet::Pia2PortB, static_cast<uint8_t>(~record.data));
        wait_via(pet::NrfdInput | pet::Ndac, pet::NrfdInput);
        // Assert DAV until accepted, then release DAV and the data lines.
        write(pet::Pia2ControlB, ieee_test::PiaC2Low.bits());
        wait_via(pet::Ndac, pet::Ndac);
        write(pet::Pia2ControlB, ieee_test::PiaC2High.bits());
        wait_via(pet::Ndac, 0);
        write(pet::Pia2PortB, ieee_test::BusReleased);
        write(pet::Pia1ControlA, ieee_test::PiaC2High.bits());
    }
    // Receives one byte and its EOI state using the PET acceptor handshake.
    ieee_test::Record receive() {
        // Arm before releasing ATN/NRFD, without reasserting ATN between bytes.
        write(pet::Pia2ControlA, ieee_test::PiaC2Low.bits());
        write(pet::ViaPortB, ieee_test::BusReleased);
        wait_via(pet::Dav, 0);
        // Block the next byte before sampling EOI and data, as KERNAL ACPTR does.
        write(pet::ViaPortB, AcceptorBusy);
        const auto eoi = (read(pet::Pia1PortA) & ieee_test::EoiInput) == 0
            ? ieee_test::Eoi::Last : ieee_test::Eoi::More;
        const bool atn = (read(pet::ViaPortB) & pet::Atn) == 0;
        const uint8_t value = static_cast<uint8_t>(~read(pet::Pia2PortA));
        // Acknowledge the byte and wait for the source to release DAV.
        write(pet::Pia2ControlA, ieee_test::PiaC2High.bits());
        wait_via(pet::Dav, pet::Dav);
        // Rearm NDAC after DAV releases, keeping NRFD asserted until the next call.
        write(pet::Pia2ControlA, ieee_test::PiaC2Low.bits());
        return ieee_test::raw_record(value, atn, eoi);
    }
private:
    enum class Access { Read, Write };
    static constexpr econopet::CpuAddress CommandMailbox{0x0203};
    static constexpr econopet::CpuAddress WriteMailbox{0x0204};
    static constexpr econopet::CpuAddress ReadMailbox{0x0205};
    enum MailboxState : uint8_t { MailboxIdle = 0, MailboxPending = 1 };
    static constexpr uint8_t AtnAsserted = ieee_test::BusReleased & ~pet::Atn;
    static constexpr uint8_t AcceptorBusy = ieee_test::BusReleased & ~pet::Nrfd;
    static constexpr econopet::Cycles StartupBudget = 2000;
    static constexpr econopet::Cycles TransferBudget = 10000;
    static constexpr econopet::Cycles HandshakeBudget = TransferBudget * 200;
    // Changes the instruction only while the CPU is parked in the mailbox loop.
    uint8_t transfer(econopet::CpuAddress address, uint8_t value, Access access) {
        if (failed_)
            throw system_.failure<std::logic_error>("IEEE controller", "reuse after interrupted transfer or handshake");
        try {
            if (system_.peek(econopet::SramAddress{CommandMailbox.value()}) != MailboxIdle)
                throw system_.failure<std::logic_error>("IEEE controller", "CPU I/O mailbox is not idle");
            // Patch the parked instruction before publishing its mailbox command.
            const auto instruction = access == Access::Write
                ? test_asm::mos6502::sta_abs(address) : test_asm::mos6502::lda_abs(address);
            for (size_t offset = 0; offset < instruction.bytes.size(); ++offset)
                system_.poke(econopet::SramAddress{(transfer_opcode_ + offset).value()}, instruction.bytes[offset]);
            system_.poke(econopet::SramAddress{WriteMailbox.value()}, value);
            system_.poke(econopet::SramAddress{CommandMailbox.value()}, MailboxPending);
            // The CPU acknowledgement makes the result safe to read.
            system_.run_until([](const System& observed) { return observed.peek(econopet::SramAddress{CommandMailbox.value()}) == MailboxIdle; },
                            TransferBudget, "CPU I/O mailbox");
            return system_.peek(econopet::SramAddress{ReadMailbox.value()});
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
    // Include all mailbox activity in a shared, elapsed-cycle handshake deadline.
    void wait_via(uint8_t mask, uint8_t expected) {
        bool ready = false;
        try {
            system_.service_until([&](const System&) { return ready; },
                [&](System&, econopet::Cycles) { ready = (read(pet::ViaPortB) & mask) == expected; },
                HandshakeBudget, "IEEE handshake: VIA mask=" + std::to_string(mask)
                    + " expected=" + std::to_string(expected));
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
    System& system_;
    econopet::CpuAddress transfer_opcode_;
    bool failed_ = false;
};
