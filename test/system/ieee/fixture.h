// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "framework/program.h"
#include "registers.h"
#include "roms.h"

// Physical controller constants and revision-bound native ROM fixtures.
namespace ieee_test {
using econopet::io::PiaC2High;
using econopet::io::PiaC2Low;
inline constexpr uint8_t BusReleased = 0xff;
inline constexpr uint8_t DioOutputs = 0xff;
inline constexpr uint8_t EoiInput = pet::EoiInput;
inline constexpr econopet::CpuAddress HarnessStart{0x0300};
inline constexpr econopet::Cycles ByteBudget = 500000;

inline constexpr uint8_t DeviceAddress = 8;
inline constexpr uint8_t LoadChannel = 0;
inline constexpr uint8_t FileChannel = 2;
inline constexpr uint8_t CommandChannel = 15;
inline constexpr uint8_t ViaIdle = pet::Nrfd | pet::Atn;
inline constexpr econopet::CpuAddress CompletionAddress{0x0200};
inline constexpr econopet::CpuAddress IntermediateAddress{0x0201};
inline constexpr econopet::CpuAddress ReleaseAddress{0x0203};
inline constexpr econopet::CpuAddress PayloadAddress{0x0400};
inline constexpr test_asm::MemoryRegion RomCodeStorage{econopet::CpuAddress{0x1000}, 0x0800};
inline constexpr test_asm::MemoryRegion WaterlooIoHooks{econopet::CpuAddress{0x0580}, 0x80};
inline constexpr econopet::Cycles KernalBudget{5000000};
inline constexpr econopet::Cycles WaterlooBudget{1000000};

// Select dedicated phase storage without silently accepting an unknown mode.
inline std::optional<econopet::CpuAddress> rom_release(test_asm::ProgramMode mode) {
    switch (mode) {
    case test_asm::ProgramMode::Complete: return std::nullopt;
    case test_asm::ProgramMode::Phased: return ReleaseAddress;
    }
    throw std::invalid_argument("ROM layout requires complete or phased mode");
}

// Describe every PET reservation once for explicit scenarios and board-backed builders.
inline test_asm::LayoutOverrides kernal_layout(test_asm::ProgramMode mode,
                                               test_asm::MemoryRegion code = RomCodeStorage) {
    test_asm::LayoutOverrides layout;
    layout.stack = test_asm::MemoryRegion{econopet::CpuAddress{0x0100}, 0x100};
    layout.results = test_asm::MemoryRegion{IntermediateAddress, 2};
    layout.code = code;
    layout.vectors = test_asm::MemoryRegion{econopet::CpuAddress{0xfffa}, 6};
    layout.completion = CompletionAddress;
    layout.release = rom_release(mode);
    const bool phased = layout.release.has_value();
    layout.workspace = {
        {"zero page", {econopet::CpuAddress{0}, 0x100}},
        {"KERNAL tables", {ReleaseAddress + (phased ? 1 : 0), phased ? 0xfcu : 0xfdu}},
        {"editor high RAM", {econopet::CpuAddress{0x03fc}, 4}}
    };
    layout.data = {{"payload", {PayloadAddress, 0x100}}};
    return layout;
}

// Describe the native Waterloo parameter block, stack, vectors and I/O hook tables.
inline test_asm::LayoutOverrides waterloo_layout(test_asm::ProgramMode mode,
                                                 test_asm::MemoryRegion code = RomCodeStorage) {
    test_asm::LayoutOverrides layout;
    layout.stack = test_asm::MemoryRegion{econopet::CpuAddress{0x0180}, 0x80};
    layout.results = test_asm::MemoryRegion{IntermediateAddress, 2};
    layout.code = code;
    layout.vectors = test_asm::MemoryRegion{econopet::CpuAddress{0xfff0}, 0x10};
    layout.completion = CompletionAddress;
    layout.release = rom_release(mode);
    layout.workspace = {
        {"direct page", {econopet::CpuAddress{0}, 0x100}},
        {"control block and filename", {econopet::CpuAddress{0x0100}, 0x80}},
        {"I/O hooks", WaterlooIoHooks}
    };
    return layout;
}

// Consume a complete ROM layout without restating family-specific addresses or sizes.
template<class Memory>
class RomScenario : public test_asm::Scenario<Memory> {
public:
    const test_asm::Region code, results, completion_region, stack;
protected:
    // Reserve the same named regions and purposes accepted by ProgramBuilder.
    RomScenario(Memory& memory, const test_asm::LayoutOverrides& layout)
        : test_asm::Scenario<Memory>(memory),
          code(reserve_required("code", test_asm::RegionKind::Code, layout.code)),
          results(reserve_required("results", test_asm::RegionKind::Results, layout.results)),
          completion_region(this->reserve("completion", test_asm::RegionKind::Mailbox,
              layout.completion.value(), 1)),
          stack(reserve_required("stack", test_asm::RegionKind::Stack, layout.stack)) {
        reserve_required("vectors", test_asm::RegionKind::Vectors, layout.vectors);
        if (layout.release)
            this->reserve("release", test_asm::RegionKind::Mailbox, *layout.release, 1);
        for (const auto& [name, region] : layout.workspace)
            this->reserve(name, test_asm::RegionKind::Workspace, region.start, region.size);
        for (const auto& [name, region] : layout.data)
            this->reserve(name, test_asm::RegionKind::Data, region.start, region.size);
    }
private:
    // Reject incomplete ROM descriptions instead of borrowing generic layout defaults.
    test_asm::Region reserve_required(const std::string& name, test_asm::RegionKind kind,
                                     const std::optional<test_asm::MemoryRegion>& region) {
        if (!region)
            throw this->template failure<std::invalid_argument>("ROM layout", "missing reservation " + name);
        return this->reserve(name, kind, region->start, region->size);
    }
};

// Reserve PET workspace separately from a configurable native 6502 harness.
template<class Memory>
class KernalScenario : public RomScenario<Memory> {
public:
    // Consume the canonical complete layout, retaining ordinary payload ownership.
    explicit KernalScenario(Memory& memory, test_asm::MemoryRegion code = RomCodeStorage)
        : RomScenario<Memory>(memory, kernal_layout(test_asm::ProgramMode::Complete, code)),
          payload(this->region("payload")) {}
    const test_asm::Region payload;
};
template<class Memory> KernalScenario(Memory&) -> KernalScenario<Memory>;

// Reserve the private descending 6809 stack above the Waterloo parameter block.
template<class Memory>
class WaterlooScenario : public RomScenario<Memory> {
public:
    // Consume the canonical complete layout, including every native I/O hook byte.
    explicit WaterlooScenario(Memory& memory, test_asm::MemoryRegion code = RomCodeStorage)
        : RomScenario<Memory>(memory, waterloo_layout(test_asm::ProgramMode::Complete, code)) {}
    // Return the exclusive upper bound used by the native descending stack.
    econopet::CpuAddress stack_top() const { return this->stack.start() + this->stack.size(); }
};
template<class Memory> WaterlooScenario(Memory&) -> WaterlooScenario<Memory>;

enum class KernalRoutine {
    Cint, Talk, Listen, List1, Isour, Secondary, Scatn, Er001, Tksa, Tkatn,
    Ciout, Untalk, Unlisten, Acptr, Openi, Clse1, Clos5, Trans1,
    Chkin, Chkout, Clrchn, Clall
};
enum class WaterlooRoutine {
    IeeeInit, ClearIoHooks, AtnDown, AtnUp, EoiDown, EoiUp, Unlisten, Untalk, Listen, Talk,
    EoiByte, FinalByte, ByteOut, Open, Close, RelOpen, Scratch, Rename,
    Initialize, Input, RelPosition, Status
};

// PET prepared-state locations, including ROM 4's separate BOB timeout-control byte.
struct KernalWorkspace {
    static constexpr econopet::CpuAddress Status{0x0096}, BufferedByte{0x00a0},
        SourceByte{0x00a5}, FileCount{0x00ae}, DefaultInput{0x00af}, DefaultOutput{0x00b0},
        FilenameLength{0x00d1}, Secondary{0x00d3}, Device{0x00d4}, Filename{0x00da},
        LogicalFiles{0x0251}, Devices{0x025b}, Secondaries{0x0265}, IeeeTimeoutControl{0x03fc};
};

// Waterloo transport routines use a parameter block, not an application FCB.
struct WaterlooWorkspace {
    static constexpr econopet::CpuAddress ActiveBlock{0x0002}, FilenameCursor{0x0068},
        Block{0x0100}, DeviceCode{0x0102}, RecordSizeHigh{0x0105}, RecordSizeLow{0x0106},
        WaitParameterHigh{0x0107}, WaitParameterLow{0x0108},
        Primary{0x0109}, Secondary{0x010a}, TransferState{0x010b}, DiskFlags{0x010c},
        Filename{0x0140};
    static constexpr uint8_t DiskDevice = 0x81, DefaultRecordSize = 1, DiskChannelMask = 0x40;
    static constexpr size_t FilenameCapacity = 0x40;
};

struct RoutineEntry {
    econopet::CpuAddress address;
    const char* name;
};

// Require the declared revision while still allowing deliberate byte patches.
inline void require_revision(const System& board, test_rom::Set revision, const std::string& operation) {
    if (!board.selected_rom() || board.selected_rom()->set != revision)
        throw board.failure<std::logic_error>(operation, "selected ROM no longer matches "
            + std::string(test_rom::metadata(revision).identity));
}

// Bind the checked System-backed CPU runner to one ROM independently of the fixture.
template<class Cpu>
class RomProgramBuilder {
public:
    // Establish revision-specific reservations and phase storage before allocation.
    RomProgramBuilder(System& board, test_rom::Set revision, const test_asm::LayoutOverrides& layout,
                      test_asm::ProgramMode mode)
        : board_(board), revision_(revision), builder_(board, test_asm::Layout::Pet, layout, mode) {
        require_selected();
        constexpr auto selected_cpu = std::is_same_v<Cpu, test_asm::Mos6502> ? CPU_SOFT_6502 : CPU_SOFT_6809;
        if (test_rom::metadata(revision).cpu != selected_cpu)
            throw board_.failure<std::invalid_argument>("ROM program", "CPU does not match the selected revision");
    }
    RomProgramBuilder(System&&, test_rom::Set, const test_asm::LayoutOverrides&, test_asm::ProgramMode) = delete;
    RomProgramBuilder(const RomProgramBuilder&) = delete;
    RomProgramBuilder& operator=(const RomProgramBuilder&) = delete;
    RomProgramBuilder(RomProgramBuilder&&) = delete;
    RomProgramBuilder& operator=(RomProgramBuilder&&) = delete;

    // Allocate a private capture byte without emitting ROM setup.
    auto byte(std::string name, test_asm::Capture capture = test_asm::Capture::Yes) {
        require_selected();
        return builder_.byte(std::move(name), capture);
    }
    // Allocate a native-endian private capture word.
    auto word(std::string name, test_asm::Capture capture = test_asm::Capture::Yes) {
        require_selected();
        return builder_.word(std::move(name), capture);
    }
    // Allocate ordinary results with an independent capture policy.
    auto buffer(std::string name, size_t size, test_asm::Capture capture = test_asm::Capture::Yes) {
        require_selected();
        return builder_.buffer(std::move(name), size, capture);
    }
    // Reserve additional disjoint ordinary data.
    auto data(std::string name, econopet::CpuAddress start, size_t size,
              test_asm::Capture capture = test_asm::Capture::No) {
        require_selected();
        return builder_.data(std::move(name), start, size, capture);
    }
    // Select revision-owned workspace for explicit initialization or observation.
    auto lower_bank(const std::string& name) {
        require_selected();
        return builder_.lower_bank(name);
    }
    // Opt an owned buffer into the normalized capture plan.
    void capture(const test_asm::MemoryBuffer& data) {
        require_selected();
        builder_.capture(data);
    }
    // Capture an owned byte without accepting unrelated types or phase handles.
    void capture(const test_asm::MemoryByte& data) {
        require_selected();
        builder_.capture(data);
    }
    // Capture both bytes of an owned word.
    void capture(const test_asm::MemoryWord& data) {
        require_selected();
        builder_.capture(data);
    }
    // Configure the single elapsed startup/execution budget.
    RomProgramBuilder& within(econopet::Cycles budget) {
        require_selected();
        builder_.within(budget);
        return *this;
    }
    // Configure exact relative input stimulus using the shared run vocabulary.
    RomProgramBuilder& after(econopet::Cycles delay, RunConfiguration::Stimulus work,
                            RunAnchor anchor = RunAnchor::RunEntry) {
        require_selected();
        builder_.after(delay, std::move(work), anchor);
        return *this;
    }
    // Configure anchored or after-slice work through the underlying budget owner.
    template<class Policy, class Service, class... Anchor>
    RomProgramBuilder& every(econopet::Cycles period, Policy policy, Service work, Anchor... anchor) {
        require_selected();
        builder_.every(period, policy, std::move(work), anchor...);
        return *this;
    }
    // Add a predicate without bypassing mandatory CPU completion or acknowledgment.
    RomProgramBuilder& until(RunConfiguration::Predicate done) {
        require_selected();
        builder_.until(std::move(done));
        return *this;
    }
    // Check identity before assembly, after assembly, and throughout execution.
    template<class Assemble, class... Work>
    test_asm::ExecutionResult run(Assemble assemble, Work&&... work) {
        return checked([&] {
            return builder_.run([&](auto& p) { assemble(p); require_selected(); },
                std::forward<Work>(work)...);
        });
    }
    // Start a construction-time phased program under the same identity checks.
    template<class Assemble, class... Work>
    test_asm::PhaseHandle pause(Assemble assemble, Work&&... work) {
        return checked([&] {
            return builder_.pause([&](auto& p) { assemble(p); require_selected(); },
                std::forward<Work>(work)...);
        });
    }
    // Resume without changing the ROM, reset lifecycle or private phase capabilities.
    template<class... Work>
    test_asm::PhaseHandle resume(test_asm::PhaseHandle& phase, Work&&... work) {
        return checked([&] { return builder_.resume(phase, std::forward<Work>(work)...); });
    }
    // Return the frozen acknowledged data only while this revision is selected.
    test_asm::ExecutionResult capture(const test_asm::PhaseHandle& phase) const {
        require_selected();
        return builder_.capture(phase);
    }
private:
    // Share identity enforcement across every terminal operation and service style.
    template<class Operation>
    auto checked(Operation operation) {
        require_selected();
        auto subscription = board_.observe([this](const System&) { require_selected(); });
        auto result = operation();
        require_selected();
        return result;
    }
    // Bind revision checks to the borrowed board rather than the fixture lifetime.
    void require_selected() const { require_revision(board_, revision_, "ROM program"); }
    System& board_;
    const test_rom::Set revision_;
    test_asm::ProgramBuilder<Cpu> builder_;
};

// Bind Commodore routine entries and prepared-state workspace to a real board.
class KernalFixture : public KernalScenario<System> {
public:
    using Workspace = KernalWorkspace;
    using Cpu = test_asm::Mos6502;
    // Load an absent set or adopt the exact selected set without replacing it.
    KernalFixture(System& board, test_rom::Set revision, test_asm::MemoryRegion code = RomCodeStorage)
        : KernalScenario<System>(board, code), board_(board), metadata_(test_rom::metadata(revision)) {
        if (!metadata_.commodore)
            throw failure<std::invalid_argument>("KERNAL fixture", "requires a Commodore revision");
        if (!board_.selected_rom()) board_.load_rom_set(revision);
        require_selected();
    }
    KernalFixture(System&&, test_rom::Set, test_asm::MemoryRegion = RomCodeStorage) = delete;
    // Reject stale fixtures but permit deliberate ROM byte patches.
    void require_selected() const { require_revision(board_, metadata_.set, "KERNAL fixture"); }
    // Resolve the selected revision's callable or explicitly prepared-state boundary.
    RoutineEntry routine(KernalRoutine selected) const {
        require_selected();
        const auto& e = *metadata_.commodore;
        switch (selected) {
        case KernalRoutine::Cint: return {e.Cint, "CINT"};
        case KernalRoutine::Talk: return {e.Talk, "TALK"};
        case KernalRoutine::Listen: return {e.Listen, "LISTN"};
        case KernalRoutine::List1: return {e.List1, "LIST1"};
        case KernalRoutine::Isour: return {e.Isour, "ISOUR"};
        case KernalRoutine::Secondary: return {e.Secondary, "SECND"};
        case KernalRoutine::Scatn: return {e.Scatn, "SCATN"};
        case KernalRoutine::Er001: return {e.Er001, "ER001"};
        case KernalRoutine::Tksa: return {e.Tksa, "TKSA"};
        case KernalRoutine::Tkatn: return {e.Tkatn, "TKATN"};
        case KernalRoutine::Ciout: return {e.Ciout, "CIOUT"};
        case KernalRoutine::Untalk: return {e.Untalk, "UNTLK"};
        case KernalRoutine::Unlisten: return {e.Unlisten, "UNLSN"};
        case KernalRoutine::Acptr: return {e.Acptr, "ACPTR"};
        case KernalRoutine::Openi: return {e.Openi, "OPENI"};
        case KernalRoutine::Clse1: return {e.Clse1, "CLSE1"};
        case KernalRoutine::Clos5: return {e.Clos5, "CLOS5"};
        case KernalRoutine::Trans1:
            if (e.Trans1) return {*e.Trans1, "TRANS1"};
            break;
        case KernalRoutine::Chkin: return {econopet::CpuAddress{0xffc6}, "CHKIN"};
        case KernalRoutine::Chkout: return {econopet::CpuAddress{0xffc9}, "CHKOUT"};
        case KernalRoutine::Clrchn: return {econopet::CpuAddress{0xffcc}, "CLRCHN"};
        case KernalRoutine::Clall: return {econopet::CpuAddress{0xffe7}, "CLALL"};
        }
        throw failure<std::invalid_argument>("KERNAL routine",
            std::string(metadata_.identity) + " has no selected routine, enum="
                + std::to_string(static_cast<int>(selected)));
    }
    // Expose the checked ROM entry for explicit low-level assembly.
    econopet::CpuAddress entry(KernalRoutine selected) const { return routine(selected).address; }
    // Couple diagnostics to the revision and selected boundary.
    std::string name(KernalRoutine selected) const {
        return std::string(metadata_.identity) + " " + routine(selected).name;
    }
    // Run a complete explicit scenario with a deadline-aware service.
    template<class Program>
    auto run(Program& p, const test_asm::Region& results, econopet::Cycles budget,
             const std::string& reason,
             RunConfiguration::Service service = [](System& board, econopet::Cycles) { board.tick(1); }) {
        RunConfiguration configuration(board_);
        configuration.within(budget).service(std::move(service));
        return run(p, results, configuration, reason);
    }
    // Keep explicit configured execution bound to the selected CPU and revision.
    template<class Program>
    auto run(Program& p, const test_asm::Region& results, RunConfiguration& configuration,
             const std::string& reason) {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        require_selected();
        auto subscription = board_.observe([this](const System&) { require_selected(); });
        auto result = KernalScenario<System>::run(p, results, configuration, reason);
        require_selected();
        return result;
    }
    // Emit editor initialization and explicit IEEE line/status preconditions.
    template<class Program>
    void init_ieee(Program& p, uint8_t via_orb = ViaIdle) const {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        p.sei().jsr(entry(KernalRoutine::Cint))
         .lda(test_asm::imm(PiaC2High.bits())).sta(pet::Pia2ControlA)
         .lda(test_asm::imm(PiaC2High.bits())).sta(pet::Pia2ControlB)
         .lda(test_asm::imm(ViaIdle)).sta(pet::ViaPortB)
         .lda(test_asm::imm(BusReleased)).sta(pet::Pia2PortB)
         .lda(test_asm::imm(0)).sta(test_asm::zp(Workspace::Status)).sta(Workspace::IeeeTimeoutControl)
         .lda(test_asm::imm(via_orb)).sta(pet::ViaPortB);
    }
    // Emit the literal device byte into the KERNAL's prepared file parameters.
    template<class Program>
    void device(Program& p, uint8_t address = DeviceAddress) const {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        require_selected();
        p.lda(test_asm::imm(address)).sta(test_asm::zp(Workspace::Device));
    }
    // Emit a counted filename and its little-endian pointer after explicit initialization.
    template<class Program>
    void file(Program& p, econopet::ByteView bytes, uint8_t secondary = FileChannel) const {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        require_selected();
        constexpr size_t FilenameCapacity = 0xff;
        if (bytes.size() == 0 || bytes.size() > FilenameCapacity)
            throw failure<std::out_of_range>("KERNAL file", "filename requires 1..255 bytes");
        const auto payload = p.lower_bank("payload").buffer("filename", 0, bytes.size());
        const auto pointer = p.lower_bank("zero page").word("filename pointer",
            Workspace::Filename.value(), test_asm::ByteOrder::LittleEndian);
        p.store(payload, bytes)
         .lda(test_asm::imm(secondary)).sta(test_asm::zp(Workspace::Secondary))
         .lda(test_asm::imm(bytes.size())).sta(test_asm::zp(Workspace::FilenameLength))
         .store_zp(pointer, payload.address());
    }
    // Create a board-backed builder with mode-independent results and private phase storage.
    auto program(test_asm::ProgramMode mode = test_asm::ProgramMode::Complete) const {
        require_selected();
        return RomProgramBuilder<Cpu>(board_, metadata_.set,
            kernal_layout(mode, {code.start(), code.size()}), mode);
    }
private:
    System& board_;
    const test_rom::Metadata metadata_;
};

// Bind Waterloo revision 12 and its parameter-block ABI to the real 6809.
class WaterlooFixture : public WaterlooScenario<System> {
public:
    using Workspace = WaterlooWorkspace;
    using Cpu = test_asm::Mc6809;
    // Load or adopt revision 12 without implicitly executing IEEEInit.
    explicit WaterlooFixture(System& board, test_asm::MemoryRegion code = RomCodeStorage)
        : WaterlooScenario<System>(board, code), board_(board) {
        if (!board_.selected_rom()) board_.load_rom_set(test_rom::Set::Waterloo);
        require_selected();
    }
    WaterlooFixture(System&&, test_asm::MemoryRegion = RomCodeStorage) = delete;
    // Reject replacement while allowing deliberate byte patches within the revision.
    void require_selected() const { require_revision(board_, test_rom::Set::Waterloo, "Waterloo fixture"); }
    // Resolve only revision-12 routine boundaries from the immutable catalog.
    RoutineEntry routine(WaterlooRoutine selected) const {
        require_selected();
        const auto& e = test_rom::WaterlooRevision12;
        switch (selected) {
        case WaterlooRoutine::IeeeInit: return {e.IeeeInit, "IEEEInit"};
        case WaterlooRoutine::ClearIoHooks: return {e.ClearIoHooks, "clear system I/O hooks"};
        case WaterlooRoutine::AtnDown: return {e.AtnDown, "ATNDown"};
        case WaterlooRoutine::AtnUp: return {e.AtnUp, "ATNUp"};
        case WaterlooRoutine::EoiDown: return {e.EoiDown, "EOIDown"};
        case WaterlooRoutine::EoiUp: return {e.EoiUp, "EOIUp"};
        case WaterlooRoutine::Unlisten: return {e.Unlisten, "UNListen"};
        case WaterlooRoutine::Untalk: return {e.Untalk, "UNTalk"};
        case WaterlooRoutine::Listen: return {e.Listen, "LISTEN"};
        case WaterlooRoutine::Talk: return {e.Talk, "TALK"};
        case WaterlooRoutine::EoiByte: return {e.EoiByte, "EOI byte"};
        case WaterlooRoutine::FinalByte: return {e.FinalByte, "final byte"};
        case WaterlooRoutine::ByteOut: return {e.ByteOut, "ByteOut"};
        case WaterlooRoutine::Open: return {e.Open, "OPEN"};
        case WaterlooRoutine::Close: return {e.Close, "CLOSE"};
        case WaterlooRoutine::RelOpen: return {e.RelOpen, "REL OPEN"};
        case WaterlooRoutine::Scratch: return {e.Scratch, "SCRATCH"};
        case WaterlooRoutine::Rename: return {e.Rename, "RENAME"};
        case WaterlooRoutine::Initialize: return {e.Initialize, "initialize"};
        case WaterlooRoutine::Input: return {e.Input, "input"};
        case WaterlooRoutine::RelPosition: return {e.RelPosition, "REL position"};
        case WaterlooRoutine::Status: return {e.Status, "status"};
        }
        throw failure<std::invalid_argument>("Waterloo routine",
            "Waterloo revision 12 has no selected routine, enum=" + std::to_string(static_cast<int>(selected)));
    }
    // Expose the checked routine entry for explicit programs.
    econopet::CpuAddress entry(WaterlooRoutine selected) const { return routine(selected).address; }
    // Retain the exact revision in entry diagnostics.
    std::string name(WaterlooRoutine selected) const {
        return std::string("Waterloo revision 12 ") + routine(selected).name;
    }
    // Run an explicit scenario under one budget and service owner.
    template<class Program>
    auto run(Program& p, const test_asm::Region& results, econopet::Cycles budget,
             const std::string& reason,
             RunConfiguration::Service service = [](System& board, econopet::Cycles) { board.tick(1); }) {
        RunConfiguration configuration(board_);
        configuration.within(budget).service(std::move(service));
        return run(p, results, configuration, reason);
    }
    // Preserve selected CPU/revision checks for explicit configured execution.
    template<class Program>
    auto run(Program& p, const test_asm::Region& results, RunConfiguration& configuration,
             const std::string& reason) {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        require_selected();
        auto subscription = board_.observe([this](const System&) { require_selected(); });
        auto result = WaterlooScenario<System>::run(p, results, configuration, reason);
        require_selected();
        return result;
    }
    // Disable user I/O hooks before establishing native-ROM IEEE preconditions.
    template<class Program>
    void init_ieee(Program& p) const {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        p.jsr(entry(WaterlooRoutine::ClearIoHooks));
        init_ieee_lines(p);
    }
    // Emit only IEEEInit, preserving hook state for deliberately customized I/O tests.
    template<class Program>
    void init_ieee_lines(Program& p) const {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        p.jsr(entry(WaterlooRoutine::IeeeInit));
    }
    // Emit the active parameter-block pointer and literal device/channel fields.
    template<class Program>
    void device(Program& p, uint8_t address = DeviceAddress, uint8_t secondary = LoadChannel) const {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        require_selected();
        p.ldx(test_asm::imm16(Workspace::Block)).stx(test_asm::dp(Workspace::ActiveBlock))
         .clr_x().clr_x_offset(Workspace::WaitParameterHigh - Workspace::Block)
         .clr_x_offset(Workspace::WaitParameterLow - Workspace::Block)
         .lda(test_asm::imm(address)).sta(Workspace::Primary)
         .lda(test_asm::imm(secondary)).sta(Workspace::Secondary);
    }
    // Emit disk fields, a big-endian cursor and caller-supplied filename bytes without host writes.
    template<class Program>
    void file(Program& p, econopet::ByteView bytes) const {
        static_assert(std::is_same_v<typename Program::CpuType, Cpu>);
        require_selected();
        if (bytes.size() == 0 || bytes.size() > Workspace::FilenameCapacity)
            throw failure<std::out_of_range>("Waterloo file", "filename requires 1..64 bytes");
        const auto filename = p.lower_bank("control block and filename").buffer("filename",
            Workspace::Filename - Workspace::Block, bytes.size());
        const auto cursor = p.lower_bank("direct page").word("filename cursor",
            Workspace::FilenameCursor.value(), test_asm::ByteOrder::BigEndian);
        p.lda(test_asm::imm(Workspace::DiskDevice)).sta(Workspace::DeviceCode)
         .lda(test_asm::imm(0)).sta(Workspace::RecordSizeHigh).sta(Workspace::TransferState)
         .lda(test_asm::imm(Workspace::DefaultRecordSize)).sta(Workspace::RecordSizeLow)
         .lda(test_asm::imm(Workspace::DiskChannelMask)).sta(Workspace::DiskFlags)
         .store(cursor, filename.address()).store(filename, bytes);
    }
    // Create a complete or phased runner with the ROM parameter block below its private stack.
    auto program(test_asm::ProgramMode mode = test_asm::ProgramMode::Complete) const {
        require_selected();
        return RomProgramBuilder<Cpu>(board_, test_rom::Set::Waterloo,
            waterloo_layout(mode, {code.start(), code.size()}), mode);
    }
private:
    System& board_;
};
} // namespace ieee_test
