// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <algorithm>
#include <utility>

#include "framework/asm.h"
#include "framework/execution.h"
#include "framework/run.h"
#include "system.h"

namespace test_asm {

template<class Cpu, class Memory> class Assembly;

// Assembly-only access cannot install, execute or choose a completion marker.
template<class Cpu, class Memory>
class Assembly : public FluentInstructions<Cpu, Assembly<Cpu, Memory>> {
public:
    using CpuType = Cpu;
    using FluentInstructions<Cpu, Assembly>::sta;
    using FluentInstructions<Cpu, Assembly>::lda;
    Assembly(const Assembly&) = delete;
    Assembly& operator=(const Assembly&) = delete;
    Assembly(Assembly&&) = delete;
    Assembly& operator=(Assembly&&) = delete;

    // Emits an instruction belonging to this CPU and returns the fluent assembler.
    template<size_t Size>
    Assembly& emit(const Instruction<Cpu, Size>& instruction) {
        program_.emit(instruction);
        return *this;
    }
    // Emits a CPU-specific label relocation with the usual range checks.
    Assembly& emit(const LabelInstruction<Cpu>& instruction) {
        program_.emit(instruction);
        return *this;
    }
    // Keeps deliberate illegal-opcode emission explicit and bounds-checked.
    Assembly& raw_bytes(econopet::ByteView bytes) {
        program_.raw_bytes(bytes);
        return *this;
    }
    // Selects a reserved workspace for owned CPU operands.
    MemoryView<Memory> lower_bank(const std::string& name) {
        return program_.lower_bank(name);
    }
    // Creates a forward target in this CPU-specific program.
    Label<Cpu> label(std::string name) { return program_.label(std::move(name)); }
    // Binds a forward target at the current cursor.
    void bind(const Label<Cpu>& target) { program_.bind(target); }
    // Creates a bound target at the current cursor.
    Label<Cpu> mark(std::string name) { return program_.mark(std::move(name)); }
    // Returns a checked label address for explicit instruction operands.
    econopet::CpuAddress address(const Label<Cpu>& target) const { return program_.address(target); }

private:
    template<class> friend class ProgramBuilder;
    template<class> friend class OwnedInstructions;
    template<class, class> friend class FluentInstructions;
    explicit Assembly(Program<Cpu, Memory>& program) : program_(program) {}
    // Supplies ownership checks to the shared fluent data instructions.
    void require_data(const MemoryByte& byte) const { program_.require_data(byte); }
    // Adds board evidence to shared data instruction failures.
    template<class Exception>
    Exception data_failure(const std::string& operation, const std::string& detail) const {
        return program_.template data_failure<Exception>(operation, detail);
    }
    // Preflight a complete compound instruction before emission.
    void require_space(size_t size) const { program_.require_space(size); }
    Program<Cpu, Memory>& program_;
};

// A single-use complete CPU program backed by the production-top simulated board.
template<class Cpu>
class ProgramBuilder {
    static_assert(std::is_same_v<Cpu, Mos6502> || std::is_same_v<Cpu, Mc6809>);
public:
    // Validates all reservations without changing memory, reset or simulation time.
    ProgramBuilder(System& memory, Layout layout, const LayoutOverrides& overrides = {})
        : memory_(memory), scenario_(memory),
          stack_(reserve(layout, overrides.stack, "stack", RegionKind::Stack,
              {econopet::CpuAddress{0x0100}, 0x100})),
          results_(reserve(layout, overrides.results, "results", RegionKind::Results,
              {econopet::CpuAddress{0x0200}, 0xff})),
          vectors_(reserve(layout, overrides.vectors, "vectors", RegionKind::Vectors,
              {econopet::CpuAddress{0xfff0}, 16})),
          marker_(scenario_.completion(scenario_.reserve("completion", RegionKind::Mailbox,
              overrides.completion.value_or(econopet::CpuAddress{0x02ff}), 1))),
          code_(reserve(layout, overrides.code, "code", RegionKind::Code,
              {econopet::CpuAddress{0x0300}, 0x100})),
          program_(scenario_, code_), configuration_(memory) {
        memory_.require_mutable_access();
        if constexpr (std::is_same_v<Cpu, Mos6502>) {
            if (stack_.start() != econopet::CpuAddress{0x0100} || stack_.size() != 0x100)
                throw scenario_.template failure<std::invalid_argument>("program layout",
                    "6502 hardware stack requires the full $0100-$01ff page");
        } else {
            if (stack_.start().value() + stack_.size() >= econopet::CpuAddressCapacity)
                throw scenario_.template failure<std::out_of_range>("program layout",
                    "6809 descending stack needs a representable one-past pointer");
        }
        constexpr auto reset_vector = (std::is_same_v<Cpu, Mos6502> ? pet::Reset6502 : pet::Reset6809).value();
        if (vectors_.start().value() > reset_vector
            || vectors_.start().value() + vectors_.size() < reset_vector + 2)
            throw scenario_.template failure<std::invalid_argument>("program layout",
                "vectors must contain the selected CPU reset vector");
        for (const auto& [name, region] : overrides.workspace)
            scenario_.reserve(name, RegionKind::Workspace, region.start, region.size);
    }
    ProgramBuilder(const ProgramBuilder&) = delete;
    ProgramBuilder(System&&, Layout, const LayoutOverrides& = {}) = delete;
    ProgramBuilder& operator=(const ProgramBuilder&) = delete;
    ProgramBuilder(ProgramBuilder&&) = delete;
    ProgramBuilder& operator=(ProgramBuilder&&) = delete;

    // Allocates one named capture byte, rejecting duplicate names and exhaustion.
    MemoryByte byte(std::string name, Capture capture = Capture::Yes) {
        return buffer(std::move(name), 1, capture).byte(0);
    }
    // Allocates a word using this CPU's native pointer storage order.
    MemoryWord word(std::string name, Capture capture = Capture::Yes) {
        return buffer(std::move(name), 2, capture).word(0,
            std::is_same_v<Cpu, Mos6502> ? ByteOrder::LittleEndian : ByteOrder::BigEndian);
    }
    // Allocates ordinary owned data, with capture as an independent policy.
    MemoryBuffer buffer(std::string name, size_t size, Capture capture = Capture::Yes) {
        require_open("program buffer");
        if (name.empty())
            throw scenario_.template failure<std::invalid_argument>("program buffer", "name must not be empty");
        for (const auto& existing : names_)
            if (existing == name)
                throw scenario_.template failure<std::invalid_argument>("program buffer", "duplicate name " + name);
        if (size == 0 || size > results_.size() - allocated_)
            throw scenario_.template failure<std::out_of_range>("program buffer", "data arena is full");
        if (capture != Capture::Yes && capture != Capture::No)
            throw scenario_.template failure<std::invalid_argument>("program buffer", "unknown capture policy");
        auto result = scenario_.lower_bank(results_).buffer(name, allocated_, size);
        if (capture == Capture::Yes) captured_buffers_.push_back(result);
        allocated_ += size;
        names_.push_back(std::move(name));
        return result;
    }
    // Reserves external data without requiring a Results or Mailbox purpose.
    MemoryBuffer data(std::string name, econopet::CpuAddress start, size_t size,
                      Capture capture = Capture::No) {
        require_open("program data");
        for (const auto& existing : names_)
            if (existing == name)
                throw scenario_.template failure<std::invalid_argument>("program data", "duplicate name " + name);
        if (capture != Capture::Yes && capture != Capture::No)
            throw scenario_.template failure<std::invalid_argument>("program data", "unknown capture policy");
        auto region = scenario_.reserve(name, RegionKind::Data, start, size);
        auto result = scenario_.lower_bank(region).buffer(name, 0, size);
        if (capture == Capture::Yes) captured_buffers_.push_back(result);
        names_.push_back(std::move(name));
        return result;
    }
    // Binds a reserved workspace once for explicit host initialization and reads.
    MemoryView<System> lower_bank(const std::string& reservation = "results") {
        require_open("program lower bank");
        return scenario_.lower_bank(scenario_.region(reservation));
    }
    // Opts an existing workspace sub-buffer into capture without a new reservation.
    void capture(const MemoryBuffer& buffer) {
        require_open("program capture data");
        scenario_.require(buffer.byte(0));
        scenario_.require(buffer.byte(buffer.size() - 1));
        captured_buffers_.push_back(buffer);
    }
    // Opts an existing owned scalar into the same capture plan.
    void capture(const MemoryByte& byte) { capture(MemoryBuffer(byte, 1)); }
    // Opts both bytes of an existing owned word into capture.
    void capture(const MemoryWord& word) { capture(MemoryBuffer(word.byte(0), 2)); }

    // Sets the single elapsed-cycle budget shared by reset, startup and execution.
    ProgramBuilder& within(econopet::Cycles budget) {
        require_open("program within");
        configuration_.within(budget);
        return *this;
    }

    // Configure exact relative stimulus without another scheduler owner.
    ProgramBuilder& after(econopet::Cycles delay, RunConfiguration::Stimulus work,
                          RunAnchor anchor = RunAnchor::RunEntry) {
        require_open("program after");
        configuration_.after(delay, std::move(work), anchor);
        return *this;
    }
    // Share the board run vocabulary for anchored or after-slice active work.
    template<class Policy, class Service, class... Anchor>
    ProgramBuilder& every(econopet::Cycles period, Policy policy, Service service, Anchor... anchor) {
        require_open("program every");
        configuration_.every(period, policy, std::move(service), anchor...);
        return *this;
    }
    // Adds an observation condition without replacing mandatory CPU completion.
    ProgramBuilder& until(RunConfiguration::Predicate done) {
        require_open("program until");
        configuration_.until(std::move(done));
        return *this;
    }

    // Assembles, validates, completes, resets, installs, executes and captures once.
    template<class Assemble>
    ExecutionResult run(Assemble assemble) {
        return run(std::move(assemble), configuration_);
    }

    // Accepts the same configuration object used by standalone board execution.
    template<class Assemble>
    ExecutionResult run(Assemble assemble, RunConfiguration& configuration) {
        require_open("program run");
        if (&configuration != &configuration_) configuration_.require_replaceable();
        configuration.validate(memory_, true);
        consumed_ = true;
        prologue();
        Assembly<Cpu, System> assembly(program_);
        assemble(assembly);
        program_.finish(marker_);
        const std::string reason = std::is_same_v<Cpu, Mos6502> ? "6502 program run" : "6809 program run";
        auto execution = configuration.execute(memory_, reason, true, [&](auto budget, auto service, auto released, auto done) {
            return scenario_.execute_program(program_, &results_, budget, reason, service, released, done, false);
        });
        capture_data(execution);
        return execution;
    }

    // Supports scheduler/firmware work under the same startup/execution deadline.
    template<class Assemble>
    ExecutionResult run(Assemble assemble, RunConfiguration::Service service) {
        require_open("program run");
        configuration_.validate(memory_, true);
        configuration_.service(std::move(service));
        return run(std::move(assemble));
    }

private:
    // Reuses the normal runner's ISA-specific stack and interrupt initialization.
    void prologue() {
        if constexpr (std::is_same_v<Cpu, Mos6502>) {
            program_.emit(mos6502::sei());
            program_.emit(mos6502::ldx_imm(0xff));
            program_.emit(mos6502::txs());
        } else {
            program_.emit(mc6809::orcc_imm(0x50));
            program_.emit(mc6809::lds_imm(econopet::CpuAddress{stack_.start().value() + stack_.size()}));
        }
    }
    // Rejects mutation or recursive execution once terminal assembly starts.
    void require_open(const std::string& operation) const {
        memory_.require_mutable_access();
        if (consumed_) throw scenario_.template failure<std::logic_error>(operation, "builder was already consumed");
    }
    // Samples opted-in ordinary data at the same boundary without advancing time.
    void capture_data(ExecutionResult& execution) {
        // Union adjacent and overlapping selections so every fully captured operand is readable.
        std::sort(captured_buffers_.begin(), captured_buffers_.end(),
            [](const auto& a, const auto& b) { return a.address().value() < b.address().value(); });
        std::vector<MemoryBuffer> spans;
        for (const auto& buffer : captured_buffers_) {
            if (!spans.empty() && buffer.address().value() <= spans.back().address().value() + spans.back().size()) {
                const auto end = std::max(buffer.address().value() + buffer.size(),
                    spans.back().address().value() + spans.back().size());
                spans.back() = MemoryBuffer(spans.back().first_, end - spans.back().address().value());
            } else {
                spans.push_back(buffer);
            }
        }
        // Read each selected address once through the guarded backing object.
        for (const auto& buffer : spans) {
            std::vector<uint8_t> bytes(buffer.size());
            for (size_t i = 0; i < buffer.size(); ++i)
                bytes[i] = memory_.peek(econopet::SramAddress{buffer.address().value() + i});
            execution.append(buffer.address(), std::move(bytes));
        }
    }
    // Selects the checked preset while rejecting invalid enum values.
    MemoryRegion region(Layout layout, const std::optional<MemoryRegion>& override,
                        MemoryRegion fallback) const {
        if (layout != Layout::Pet)
            throw scenario_.template failure<std::invalid_argument>("program layout", "unknown layout preset");
        return override.value_or(fallback);
    }
    // Reserves an overridden or preset region using the scenario's overlap checks.
    Region reserve(Layout layout, const std::optional<MemoryRegion>& override,
                   const char* name, RegionKind kind, MemoryRegion fallback) {
        const auto selected = region(layout, override, fallback);
        return scenario_.reserve(name, kind, selected.start, selected.size);
    }

    System& memory_;
    Scenario<System> scenario_;
    Region stack_, results_, vectors_;
    CompletionMarker marker_;
    Region code_;
    Program<Cpu, System> program_;
    RunConfiguration configuration_;
    std::vector<std::string> names_;
    size_t allocated_ = 0;
    std::vector<MemoryBuffer> captured_buffers_;
    bool consumed_ = false;
};

} // namespace test_asm

inline test_asm::ProgramBuilder<test_asm::Mos6502> System::program6502(
    test_asm::Layout layout, const test_asm::LayoutOverrides& overrides) & {
    require_mutable_access();
    return {*this, layout, overrides};
}

inline test_asm::ProgramBuilder<test_asm::Mc6809> System::program6809(
    test_asm::Layout layout, const test_asm::LayoutOverrides& overrides) & {
    require_mutable_access();
    return {*this, layout, overrides};
}
