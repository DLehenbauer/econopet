// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <algorithm>
#include <array>
#include <utility>

#include "framework/asm.h"
#include "framework/execution.h"
#include "framework/phase.h"
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

    // Preassemble a register-preserving closed-gate handshake in phased mode only.
    Assembly& pause(std::string name) {
        if (!pauses_)
            throw data_failure<std::logic_error>("program pause", "use builder.pause for phased execution");
        if (name.empty() || name == TerminalPhase)
            throw data_failure<std::invalid_argument>("program pause", "requires a nonempty, nonterminal name");
        append_pause(std::move(name));
        return *this;
    }
private:
    template<class> friend class ProgramBuilder;
    template<class> friend class OwnedInstructions;
    template<class, class> friend class FluentInstructions;
    explicit Assembly(Program<Cpu, Memory>& program, std::vector<PausePoint>* pauses = nullptr,
                      econopet::CpuAddress acknowledgment = {}, econopet::CpuAddress release = {})
        : program_(program), pauses_(pauses), acknowledgment_(acknowledgment), release_(release) {}
    static constexpr const char* TerminalPhase = "complete";
    static constexpr size_t MaximumPhaseTokens = std::numeric_limits<uint8_t>::max();
    // Save A/flags on the native stack, publish a token and poll until explicitly released.
    void append_pause(std::string name) {
        for (const auto& point : *pauses_)
            if (point.name == name)
                throw data_failure<std::invalid_argument>("program pause", "duplicate phase " + name);
        if (pauses_->size() == MaximumPhaseTokens)
            throw data_failure<std::out_of_range>("program pause", "phase tokens exhausted");
        require_space(phase_detail::HandshakeSize);
        const auto token = static_cast<uint8_t>(pauses_->size() + 1);
        const auto gate_load = program_.address() + phase_detail::GateLoadOffset;
        emit(phase_detail::handshake<Cpu>(acknowledgment_, release_, token));
        pauses_->push_back({std::move(name), token, gate_load});
    }
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
    std::vector<PausePoint>* pauses_;
    econopet::CpuAddress acknowledgment_, release_;
};

// A single-use CPU program with construction-time storage backed by the simulated board.
template<class Cpu>
class ProgramBuilder {
    static_assert(std::is_same_v<Cpu, Mos6502> || std::is_same_v<Cpu, Mc6809>);
public:
    // Validates all reservations without changing memory, reset or simulation time.
    ProgramBuilder(System& memory, Layout layout, const LayoutOverrides& overrides = {},
                   ProgramMode mode = ProgramMode::Complete)
        : memory_(memory), scenario_(memory), mode_(mode),
          stack_(reserve(layout, overrides.stack, "stack", RegionKind::Stack,
              {econopet::CpuAddress{0x0100}, 0x100})),
          results_(reserve(layout, overrides.results, "results", RegionKind::Results,
              {econopet::CpuAddress{0x0200}, mode == ProgramMode::Phased ? 0xfeu : 0xffu})),
          vectors_(reserve(layout, overrides.vectors, "vectors", RegionKind::Vectors,
              {econopet::CpuAddress{0xfff0}, 16})),
          marker_(scenario_.completion(scenario_.reserve("completion", RegionKind::Mailbox,
              overrides.completion.value_or(econopet::CpuAddress{0x02ff}), 1))),
          release_mailbox_(mode == ProgramMode::Phased
              ? std::optional{scenario_.reserve("release", RegionKind::Mailbox,
                  overrides.release.value_or(econopet::CpuAddress{0x02fe}), 1)}
              : std::nullopt),
          code_(reserve(layout, overrides.code, "code", RegionKind::Code,
              {econopet::CpuAddress{0x0300}, 0x100})),
          program_(scenario_, code_), configuration_(memory) {
        memory_.require_mutable_access();
        if (mode != ProgramMode::Complete && mode != ProgramMode::Phased)
            throw scenario_.failure<std::invalid_argument>("program layout", "unknown program mode");
        if (mode == ProgramMode::Complete && overrides.release)
            throw scenario_.failure<std::invalid_argument>("program layout", "release override requires phased mode");
        if (release_mailbox_) release_ = release_mailbox_->start();
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
    ProgramBuilder(System&&, Layout, const LayoutOverrides& = {}, ProgramMode = ProgramMode::Complete) = delete;
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

    // Assemble all phases and start once, returning after the first verified closed gate.
    template<class Assemble>
    PhaseHandle pause(Assemble assemble) {
        return pause(std::move(assemble), configuration_);
    }
    // Use the complete runner's configuration owner for startup and first acknowledgment.
    template<class Assemble>
    PhaseHandle pause(Assemble assemble, RunConfiguration& configuration) {
        require_open("program pause");
        if (&configuration != &configuration_) configuration_.require_replaceable();
        configuration.validate(memory_, true);
        require_pause_storage();
        consumed_ = true;
        prologue();
        Assembly<Cpu, System> assembly(program_, &pauses_, marker_.address(), release_);
        assemble(assembly);
        assembly.append_pause(Assembly<Cpu, System>::TerminalPhase);
        // Keep the monitor scoped to the builder, including while the host dwells at a gate.
        subscription_.emplace(memory_.observe([this](const System& board) { sample_pause(board); }));
        const auto reason = "program pause " + pauses_.front().name;
        try {
            configuration.execute(memory_, reason, true, [&](auto budget, auto service, auto released, auto done) {
                started_ = phase_started_ = memory_.time();
                return scenario_.start_program(program_, &results_, marker_, budget, reason, service, released,
                    [&](const System& board) { return reached() && done(board); },
                    [&] {
                        constexpr std::array<uint8_t, 1> ClosedGate{0};
                        scenario_.write(*release_mailbox_, 0, ClosedGate);
                        prepared_ = true;
                    }, false);
            });
            return acknowledge();
        } catch (...) {
            fail_phases();
            throw;
        }
    }
    // Adapt polling work through the same configured phase startup lifecycle.
    template<class Assemble>
    PhaseHandle pause(Assemble assemble, RunConfiguration::Service service) {
        require_open("program pause");
        configuration_.validate(memory_, true);
        require_pause_storage();
        configuration_.service(std::move(service));
        return pause(std::move(assemble));
    }
    // Open the current gate under a fresh configuration without resetting or replacing code.
    PhaseHandle resume(PhaseHandle& phase, RunConfiguration& configuration) {
        memory_.require_mutable_access();
        require_phase(phase, "program resume");
        if (phase.complete())
            throw phase_failure("program resume", "terminal phase cannot be released");
        configuration.validate(memory_, true);
        memory_.require_spi_idle();
        const auto reason = "program resume " + pauses_[current_ + 1].name;
        phase_started_ = memory_.time();
        try {
            return configuration.execute(memory_, reason, true, [&](auto budget, auto service, auto released, auto done) {
                bool opened = false;
                memory_.service_until([&](const System& board) {
                    return opened && reached() && done(board);
                }, [&](System& board, econopet::Cycles remaining) {
                    if (!opened) {
                        // Invalidate the old capability before releasing its preassembled continuation.
                        const auto token = pauses_[current_].token;
                        ++current_;
                        acknowledgment_stage_ = AcknowledgmentStage::AwaitingWrite;
                        paused_result_.reset();
                        phase.owner_.reset();
                        board.poke(econopet::SramAddress{marker_.address().value()}, marker_.pending());
                        board.poke(econopet::SramAddress{release_.value()}, token);
                        opened = true;
                        released();
                    }
                    service(board, remaining);
                }, budget, reason);
                return acknowledge();
            });
        } catch (...) {
            fail_phases();
            throw;
        }
    }
    // Resume with a single new elapsed budget and ordinary cycle polling.
    PhaseHandle resume(PhaseHandle& phase, econopet::Cycles budget) {
        RunConfiguration configuration(memory_);
        configuration.within(budget);
        return resume(phase, configuration);
    }
    // Adapt per-pass service to the same configured gate release and deadline.
    PhaseHandle resume(PhaseHandle& phase, econopet::Cycles budget, RunConfiguration::Service service) {
        RunConfiguration configuration(memory_);
        configuration.within(budget).service(std::move(service));
        return resume(phase, configuration);
    }
    // Copy detached data sampled at the current acknowledgment, never resampling after host dwell.
    ExecutionResult capture(const PhaseHandle& phase) const {
        require_phase(phase, "program capture");
        return *paused_result_;
    }

private:
    enum class AcknowledgmentStage { AwaitingWrite, AwaitingFetch, AwaitingRead, Parked };
    enum class PhaseFailure {
        None, ClockFault, TransportFault, Reset, CpuSelection, AcknowledgmentAltered, GateOpened, Execution
    };
    // Fail storage preflight without consuming an otherwise usable complete-program builder.
    void require_pause_storage() const {
        if (mode_ != ProgramMode::Phased)
            throw scenario_.failure<std::logic_error>("program pause", "select ProgramMode::Phased at construction");
        constexpr size_t SavedRegisterBytes = 2;
        if (stack_.size() < SavedRegisterBytes)
            throw scenario_.failure<std::out_of_range>("program pause", "stack needs space for saved A and flags");
    }
    // Observe real selected-CPU bus activity, ignoring startup reset and non-CPU transport reads.
    void sample_pause(const System& board) {
        if (!prepared_ || failure_ != PhaseFailure::None) return;
        const auto pins = board.snapshot();
        if (!monitoring_) {
            if (pins.cpu_reset_active_o || pins.cpu_selection_o != std::to_underlying(SelectedCpu)) return;
            monitoring_ = true;
        }
        check_phase_invariants();
        if (failure_ != PhaseFailure::None) return;
        if (!pins.cpu_clock_o) return;
        const auto address = std::is_same_v<Cpu, Mos6502> ? pins.soft6502_addr_o : pins.soft6809_addr_o;
        const auto token = pauses_[current_].token;
        if (acknowledgment_stage_ == AcknowledgmentStage::AwaitingWrite
            && !pins.cpu_we_n_o && address == marker_.address()
            && pins.cpu_addr_o == marker_.address() && pins.cpu_data_o == token
            && !pins.ram_we_n_o && board.ram_address() == econopet::SramAddress{marker_.address().value()}) {
            acknowledgment_checkpoint_ = board.cpu(SelectedCpu).peek_fetch_checkpoint();
            acknowledgment_stage_ = AcknowledgmentStage::AwaitingFetch;
        }
        // Opcode evidence distinguishes executing the gate load from reading its code as data.
        if (acknowledgment_stage_ == AcknowledgmentStage::AwaitingFetch
            && board.cpu(SelectedCpu).peek_opcode_fetched_at(pauses_[current_].gate_load, acknowledgment_checkpoint_))
            acknowledgment_stage_ = AcknowledgmentStage::AwaitingRead;
        if (acknowledgment_stage_ == AcknowledgmentStage::AwaitingRead
            && pins.cpu_we_n_o && address == release_ && pins.cpu_addr_o == release_
            && !pins.ram_oe_n_o && board.ram_address() == econopet::SramAddress{release_.value()}
            && board.peek(econopet::SramAddress{marker_.address().value()}) == token
            && board.peek(econopet::SramAddress{release_.value()}) != token) {
            freeze_pause();
            acknowledgment_stage_ = AcknowledgmentStage::Parked;
        }
    }
    // Lifecycle failure must not be hidden by a stale acknowledgment token.
    bool reached() const {
        require_phase_health("program pause");
        return acknowledgment_stage_ == AcknowledgmentStage::Parked;
    }
    // Retain the first lifecycle or mailbox violation for observers and public operations.
    void check_phase_invariants() const {
        if (failure_ != PhaseFailure::None) return;
        const auto pins = memory_.snapshot();
        if (memory_.clock_faulted_) failure_ = PhaseFailure::ClockFault;
        else if (memory_.spi_failed_) failure_ = PhaseFailure::TransportFault;
        else if (!monitoring_) return;
        else if (memory_.reset_pin_unsettled_ || pins.cpu_reset_active_o) failure_ = PhaseFailure::Reset;
        else if (pins.cpu_selection_o != std::to_underlying(SelectedCpu)) failure_ = PhaseFailure::CpuSelection;
        else if (acknowledgment_stage_ == AcknowledgmentStage::Parked) {
            if (memory_.peek(econopet::SramAddress{marker_.address().value()}) != pauses_[current_].token)
                failure_ = PhaseFailure::AcknowledgmentAltered;
            else if (memory_.peek(econopet::SramAddress{release_.value()}) == pauses_[current_].token)
                failure_ = PhaseFailure::GateOpened;
        }
    }
    // Surface the retained cause through the same diagnostics at every phase API boundary.
    void require_phase_health(const std::string& operation) const {
        check_phase_invariants();
        switch (failure_) {
        case PhaseFailure::None: return;
        case PhaseFailure::ClockFault: throw phase_failure(operation, "hardware clock failed during phases");
        case PhaseFailure::TransportFault: throw phase_failure(operation, "SPI transport failed during phases");
        case PhaseFailure::Reset: throw phase_failure(operation, "CPU reset changed during phases");
        case PhaseFailure::CpuSelection: throw phase_failure(operation, "CPU selection changed during phases");
        case PhaseFailure::AcknowledgmentAltered: throw phase_failure(operation, "acknowledgment mailbox externally altered");
        case PhaseFailure::GateOpened: throw phase_failure(operation, "private release gate externally opened");
        case PhaseFailure::Execution: throw phase_failure(operation, "phase execution previously failed");
        }
    }
    // Freeze selected data and cumulative timing on the observer's verified closed-gate cycle.
    void freeze_pause() {
        paused_result_.emplace(scenario_.owner_, results_.start(), std::vector<uint8_t>{},
            started_, memory_.time(), memory_.trace_window(started_, memory_.time()));
        capture_data(*paused_result_);
    }
    // Issue the already frozen boundary once services and the extra predicate have completed.
    PhaseHandle acknowledge() {
        require_phase_health("program acknowledgment");
        if (!paused_result_ || acknowledgment_stage_ != AcknowledgmentStage::Parked)
            throw phase_failure("program acknowledgment", "requires a verified frozen pause");
        return PhaseHandle(owner_, current_, pauses_[current_].name, current_ + 1 == pauses_.size(),
            memory_.trace_window(phase_started_, paused_result_->completed()));
    }
    // Reject foreign, consumed, moved and externally interrupted capabilities without clocks.
    void require_phase(const PhaseHandle& phase, const std::string& operation) const {
        if (failure_ != PhaseFailure::None) require_phase_health(operation);
        if (!paused_result_ || phase.owner_ != owner_ || phase.index_ != current_)
            throw phase_failure(operation, "invalid or stale phase handle");
        require_phase_health(operation);
    }
    // Include private gate addresses and current phase in board-standard failure evidence.
    std::logic_error phase_failure(const std::string& operation, const std::string& detail) const {
        return scenario_.failure<std::logic_error>(operation, detail
            + ", phase=" + (pauses_.empty() ? "not assembled" : pauses_[current_].name)
            + ", acknowledgment_address=" + std::to_string(marker_.address().value())
            + ", release_address=" + std::to_string(release_.value()));
    }
    // Escaping phase failures retire monitoring and forbid future capture or resume.
    void fail_phases() {
        check_phase_invariants();
        if (failure_ == PhaseFailure::None) failure_ = PhaseFailure::Execution;
        subscription_.reset();
    }
    static constexpr auto SelectedCpu = std::is_same_v<Cpu, Mos6502> ? CPU_SOFT_6502 : CPU_SOFT_6809;
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
    const ProgramMode mode_;
    Region stack_, results_, vectors_;
    CompletionMarker marker_;
    std::optional<Region> release_mailbox_;
    Region code_;
    Program<Cpu, System> program_;
    RunConfiguration configuration_;
    std::vector<std::string> names_;
    size_t allocated_ = 0;
    std::vector<MemoryBuffer> captured_buffers_;
    bool consumed_ = false;
    std::shared_ptr<const int> owner_ = std::make_shared<const int>(0);
    std::vector<PausePoint> pauses_;
    econopet::CpuAddress release_;
    size_t current_ = 0;
    uint64_t acknowledgment_checkpoint_ = 0;
    bool prepared_ = false, monitoring_ = false;
    AcknowledgmentStage acknowledgment_stage_ = AcknowledgmentStage::AwaitingWrite;
    mutable PhaseFailure failure_ = PhaseFailure::None;
    econopet::CycleTime started_{econopet::Cycles{0}}, phase_started_{econopet::Cycles{0}};
    std::optional<ExecutionResult> paused_result_;
    std::optional<System::ObserverSubscription> subscription_;
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

// Construct with phase storage selected before authoring data.
inline test_asm::ProgramBuilder<test_asm::Mos6502> System::program6502(
    test_asm::ProgramMode mode, test_asm::Layout layout, const test_asm::LayoutOverrides& overrides) & {
    require_mutable_access();
    return {*this, layout, overrides, mode};
}

// Construct the native 6809 builder with the same explicit storage mode.
inline test_asm::ProgramBuilder<test_asm::Mc6809> System::program6809(
    test_asm::ProgramMode mode, test_asm::Layout layout, const test_asm::LayoutOverrides& overrides) & {
    require_mutable_access();
    return {*this, layout, overrides, mode};
}
