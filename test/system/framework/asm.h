// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "framework/memory.h"
#include "framework/trace.h"
#include "types.h"

class RunConfiguration;
namespace test_asm {

enum class Layout { Pet };
enum class ProgramMode { Complete, Phased };

struct MemoryRegion {
    econopet::CpuAddress start;
    size_t size;
};

// Explicit reservation overrides for boundary and unusual-layout tests.
struct LayoutOverrides {
    std::optional<MemoryRegion> stack;
    std::optional<MemoryRegion> results;
    std::optional<MemoryRegion> code;
    std::optional<MemoryRegion> vectors;
    std::optional<econopet::CpuAddress> completion;
    std::optional<econopet::CpuAddress> release;
    std::vector<std::pair<std::string, MemoryRegion>> workspace;
    std::vector<std::pair<std::string, MemoryRegion>> data;
};

enum class RegionKind { Code, Stack, Mailbox, Results, Vectors, Data, Workspace };

template<class Memory> class Scenario;
template<class Cpu, class Memory> class Program;
template<class Cpu> class ProgramBuilder;
template<class Cpu, class Memory> class Assembly;
enum class Capture { No, Yes };

// A reservation can only be obtained from a checked scenario layout.
class Region {
public:
    // Return the first CPU address in this reservation.
    econopet::CpuAddress start() const { return start_; }
    // Return the byte count, including a possible final byte at $FFFF.
    size_t size() const { return size_; }
    // Return an address within the reservation, never its one-past endpoint.
    econopet::CpuAddress at(size_t offset) const {
        if (offset >= size_) throw std::out_of_range("region at: offset=" + std::to_string(offset)
            + " exceeds " + name_ + ", CPU address=" + std::to_string(start_.value())
            + ", size=" + std::to_string(size_));
        return start_ + offset;
    }
    // Narrow an existing reservation without creating a new overlapping owner.
    Region slice(size_t offset, size_t size) const {
        if (!memory_detail::contains_extent(size_, offset, size))
            throw std::out_of_range("region slice: offset=" + std::to_string(offset)
                + ", requested=" + std::to_string(size) + " exceeds " + name_
                + ", CPU address=" + std::to_string(start_.value()) + ", size=" + std::to_string(size_));
        return Region(owner_, name_, kind_, at(offset), size);
    }

private:
    template<class> friend class Scenario;
    Region(std::shared_ptr<const int> owner, std::string name, RegionKind kind,
           econopet::CpuAddress start, size_t size)
        : owner_(std::move(owner)), name_(std::move(name)), kind_(kind), start_(start), size_(size) {}
    std::shared_ptr<const int> owner_;
    std::string name_;
    RegionKind kind_;
    econopet::CpuAddress start_;
    size_t size_;
};

// A dedicated mailbox issued by a checked layout, not an ordinary result byte.
class CompletionMarker {
public:
    // Return the private CPU completion address.
    econopet::CpuAddress address() const { return region_.start(); }
    // Return the freshly installed incomplete value.
    uint8_t pending() const { return pending_; }
    // Return the value emitted after all program instructions.
    uint8_t completed() const { return completed_; }
private:
    template<class> friend class Scenario;
    template<class, class> friend class Program;
    CompletionMarker(Region region, uint8_t pending, uint8_t completed)
        : region_(std::move(region)), pending_(pending), completed_(completed) {}
    Region region_;
    uint8_t pending_, completed_;
};

// Detached selected memory and the complete startup-through-completion interval.
struct ExecutionResult : MemoryCapture {
    // Own sampled bytes and a source-bound interval independently of the board.
    ExecutionResult(std::shared_ptr<const int> owner, econopet::CpuAddress start,
                    std::vector<uint8_t> bytes, econopet::CycleTime started,
                    econopet::CycleTime completed, test_observation::TraceWindow window)
        : MemoryCapture(std::move(owner), start, std::move(bytes)), started_(started), completed_(completed),
          window_(std::move(window)) {}
    // Borrow the completed run interval from a persistent result.
    const test_observation::TraceWindow& window() const & { return window_; }
    // Transfer a temporary result interval.
    test_observation::TraceWindow window() && { return std::move(window_); }
    // Copy an immutable temporary result interval.
    test_observation::TraceWindow window() const && { return window_; }
    // Report a valid run with no selected bytes.
    bool empty() const { require_live(); return spans_.empty(); }
    // Return the complete lifecycle's start before reset and selection.
    econopet::CycleTime started() const { return started_; }
    // Return the observed completion boundary without capture clocks.
    econopet::CycleTime completed() const { return completed_; }
private:
    template<class> friend class ProgramBuilder;
    // Append sampled selected data without clocks.
    void append(econopet::CpuAddress start, std::vector<uint8_t> bytes) {
        require_live();
        spans_.push_back({start, std::move(bytes)});
    }
    econopet::CycleTime started_, completed_;
    test_observation::TraceWindow window_;
};

// Record fixture-owned CPU memory before installing code or data.
// Memory supplies peek/poke and failure diagnostics (System or a host fixture).
template<class Memory>
class Scenario {
    template<class> friend class ProgramBuilder;
public:
    // Borrow persistent explicit lower-bank backing, not live CPU banking or I/O.
    explicit Scenario(Memory& memory) : memory_(memory) {}
    Scenario(Memory&&) = delete;
    Scenario(const Scenario&) = delete;
    Scenario& operator=(const Scenario&) = delete;
    Scenario(Scenario&&) = default;
    Scenario& operator=(Scenario&&) = delete;

    // Attach backing-memory evidence to rejected layout operations without clocks.
    template<class Exception>
    Exception failure(const std::string& operation, const std::string& detail) const {
        return memory_.template failure<Exception>(operation, detail);
    }
    // Reserve a named nonempty region, rejecting every overlap and CPU-space wrap.
    Region reserve(std::string name, RegionKind kind, econopet::CpuAddress start, size_t size) {
        require_live();
        if (name.empty()) throw failure<std::invalid_argument>("reserve", "fixture reservation needs a name");
        switch (kind) {
        case RegionKind::Code: case RegionKind::Stack: case RegionKind::Mailbox:
        case RegionKind::Results: case RegionKind::Vectors: case RegionKind::Data:
        case RegionKind::Workspace:
            break;
        default:
            throw failure<std::invalid_argument>("reserve", "unknown fixture region purpose");
        }
        if (size == 0 || size > econopet::CpuAddressCapacity - start.value())
            throw failure<std::out_of_range>("reserve", "fixture reservation exceeds CPU address space: "
                + name + ", address=" + std::to_string(start.value()) + ", size=" + std::to_string(size));
        for (const auto& region : regions_) {
            if (region.name_ == name)
                throw failure<std::invalid_argument>("reserve", "duplicate fixture reservation: " + name);
            if (start.value() < region.start_.value() + region.size_
                && region.start_.value() < start.value() + size)
                throw failure<std::invalid_argument>("reserve", "fixture reservation " + name + " overlaps " + region.name_);
        }
        Region region(owner_, std::move(name), kind, start, size);
        regions_.push_back(region);
        return region;
    }
    // Check reservation identity and purpose before installation or data access.
    void require(const Region& region, RegionKind kind) const {
        require_owned(region);
        if (region.kind_ != kind)
            throw failure<std::invalid_argument>("require region", "fixture region has the wrong purpose");
    }
    // Bind host data operations to ordinary data in this reservation's lower bank.
    MemoryView<Memory> lower_bank(const Region& region) const {
        require_owned(region);
        if (region.kind_ != RegionKind::Data && region.kind_ != RegionKind::Results
            && region.kind_ != RegionKind::Workspace)
            throw failure<std::invalid_argument>("lower bank", "requires an ordinary data reservation");
        return MemoryView<Memory>(memory_, owner_, region.name_, region.start(), region.size());
    }
    // Resolve a named reservation without introducing an overlapping alias.
    Region region(const std::string& name) const {
        require_live();
        for (const auto& region : regions_)
            if (region.name_ == name) return region;
        throw failure<std::invalid_argument>("region", "unknown reservation " + name);
    }
    // Check data-operand identity before fixture or later instruction operations.
    void require(const MemoryByte& byte) const {
        require_live();
        if (byte.owner_ != owner_)
            throw failure<std::invalid_argument>("memory operand", "foreign byte " + byte.name()
                + ", address=" + std::to_string(byte.address().value()));
    }
    // Install a bounded byte span without CPU decoding, SPI or clock advancement.
    void write(const Region& region, size_t offset, econopet::ByteView bytes) {
        require_owned(region);
        if (offset > region.size_ || bytes.size() > region.size_ - offset)
            throw failure<std::out_of_range>("scenario write", "fixture write exceeds " + region.name_
                + ", address=" + std::to_string(region.start().value()) + ", offset=" + std::to_string(offset)
                + ", size=" + std::to_string(bytes.size()));
        for (size_t i = 0; i < bytes.size(); ++i)
            memory_.poke(econopet::SramAddress{region.at(offset + i).value()}, bytes[i]);
    }
    // Issue a dedicated one-byte completion mailbox with distinct values.
    CompletionMarker completion(const Region& region, uint8_t pending = 0, uint8_t completed = 0x5a) const {
        require(region, RegionKind::Mailbox);
        if (region.size() != 1 || pending == completed)
            throw failure<std::invalid_argument>("completion", "needs one dedicated byte and distinct values");
        return CompletionMarker(region, pending, completed);
    }

    // Execute checked code with one captured region under the shared budget.
    template<class CpuProgram>
    ExecutionResult run(CpuProgram& program, const Region& results, econopet::Cycles budget,
                        const std::string& reason) {
        return run(program, results, budget, reason, [](Memory& board, econopet::Cycles) { board.tick(1); });
    }
    // Execute with cooperative active work and an ordinary result reservation.
    template<class CpuProgram, class Service>
    ExecutionResult run(CpuProgram& program, const Region& results, econopet::Cycles budget,
                        const std::string& reason, Service service);
    // Execute without selecting result bytes.
    template<class CpuProgram>
    ExecutionResult run(CpuProgram& program, econopet::Cycles budget, const std::string& reason);
    // Execute result-free code with active service.
    template<class CpuProgram, class Service>
    ExecutionResult run(CpuProgram& program, econopet::Cycles budget, const std::string& reason, Service service);
    // Use a shared run configuration and an ordinary result reservation.
    template<class CpuProgram>
    ExecutionResult run(CpuProgram& program, const Region& results, RunConfiguration& configuration,
                        const std::string& reason);
    // Use a shared run configuration without selected result data.
    template<class CpuProgram>
    ExecutionResult run(CpuProgram& program, RunConfiguration& configuration, const std::string& reason);

private:
    // Validate and perform the single checked startup/completion lifecycle.
    template<class CpuProgram, class Service, class Released, class Predicate>
    ExecutionResult execute_program(CpuProgram& program, const Region* results, econopet::Cycles budget,
                                    const std::string& reason, Service service, Released released, Predicate extra, bool capture_results);
    // Share validated startup between complete programs and privately monitored phase handshakes.
    template<class CpuProgram, class Service, class Released, class Predicate, class Prepared>
    ExecutionResult start_program(CpuProgram& program, const Region* results, const CompletionMarker& marker,
                                    econopet::Cycles budget, const std::string& reason, Service service, Released released,
                                    Predicate done, Prepared prepared, bool capture_results);
    // Reject all layout operations after transferring its identity to another owner.
    void require_live() const {
        if (!owner_) throw failure<std::logic_error>("layout", "fixture scenario was moved from");
    }
    // Check region identity independently of its purpose.
    void require_owned(const Region& region) const {
        require_live();
        if (region.owner_ != owner_)
            throw failure<std::invalid_argument>("memory region", "foreign or moved-from reservation");
    }
    Memory& memory_;
    std::shared_ptr<const int> owner_ = std::make_shared<const int>(0);
    std::vector<Region> regions_;
};

// CPU tags keep encoded instructions and labels separated at compile time.
struct Mos6502 {};
struct Mc6809 {};
struct ImmediateByte { uint8_t value; };
struct ImmediateWord { uint16_t value; };

// Check immediate operands before narrowing to their encoded width.
template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
constexpr ImmediateByte imm(Integer value) {
    static_assert(sizeof(Integer) <= sizeof(uint64_t));
    if constexpr (std::is_signed_v<Integer>) {
        if (value < 0) throw std::out_of_range("imm: negative byte=" + std::to_string(value));
    }
    if (static_cast<uint64_t>(value) > std::numeric_limits<uint8_t>::max())
        throw std::out_of_range("imm: byte exceeds 255, value=" + std::to_string(value));
    return {static_cast<uint8_t>(value)};
}

// Select an explicitly checked word immediate for register loads or data stores.
template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
constexpr ImmediateWord imm16(Integer value) {
    static_assert(sizeof(Integer) <= sizeof(uint64_t));
    if constexpr (std::is_signed_v<Integer>) {
        if (value < 0) throw std::out_of_range("imm16: negative word=" + std::to_string(value));
    }
    if (static_cast<uint64_t>(value) > std::numeric_limits<uint16_t>::max())
        throw std::out_of_range("imm16: word exceeds 65535, value=" + std::to_string(value));
    return {static_cast<uint16_t>(value)};
}

// Encode a checked CPU pointer without changing its address domain.
constexpr ImmediateWord imm16(econopet::CpuAddress address) { return imm16(address.value()); }

// Validate an encoded zero/direct-page byte instead of discarding its high byte.
constexpr uint8_t direct_address(econopet::CpuAddress address) {
    if (address.value() > std::numeric_limits<uint8_t>::max())
        throw std::out_of_range("direct_address: CPU address=" + std::to_string(address.value())
            + " is outside zero/direct page");
    return static_cast<uint8_t>(address.value());
}

class ZeroPage {
public:
    // Reject addresses outside the 6502 zero page.
    explicit constexpr ZeroPage(econopet::CpuAddress address) : address_(address) { direct_address(address); }
    // Return the validated encoder operand.
    constexpr econopet::CpuAddress address() const { return address_; }
private:
    econopet::CpuAddress address_;
};
// Request zero-page encoding explicitly, never automatic instruction shortening.
constexpr ZeroPage zp(econopet::CpuAddress address) { return ZeroPage(address); }

class DirectPage {
public:
    // Validate the 6809 offset byte (the live DP register selects the page).
    explicit constexpr DirectPage(econopet::CpuAddress offset) : offset_(offset) { direct_address(offset); }
    // Return the validated direct-page offset.
    constexpr econopet::CpuAddress offset() const { return offset_; }
private:
    econopet::CpuAddress offset_;
};
// Request 6809 direct-page addressing explicitly.
constexpr DirectPage dp(econopet::CpuAddress offset) { return DirectPage(offset); }

template<class Cpu, size_t Size>
struct Instruction { std::array<uint8_t, Size> bytes; };

template<class Cpu, class Memory> class Program;
template<class Derived> class OwnedInstructions;
template<class Cpu, class Derived> class FluentInstructions;

// A label belongs to one CPU and one program, even if names are reused.
template<class Cpu>
class Label {
    template<class, class> friend class Program;
    struct State {
        std::string name;
        std::optional<econopet::CpuAddress> address;
    };
    explicit Label(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

enum class Relocation { Relative8, LittleEndian16, BigEndian16 };
template<class Cpu>
struct LabelInstruction {
    uint8_t opcode;
    Label<Cpu> target;
    Relocation relocation;
};


namespace mos6502 {
template<size_t Size> using Instruction = test_asm::Instruction<Mos6502, Size>;
using Label = test_asm::Label<Mos6502>;
enum Opcode : uint8_t {
    Sei = 0x78, Cli = 0x58, Rti = 0x40, Rts = 0x60, Txs = 0x9a,
    LdaImmediate = 0xa9, LdaZeroPage = 0xa5, LdaAbsolute = 0xad, LdxImmediate = 0xa2,
    StaZeroPage = 0x85, StaAbsolute = 0x8d, JsrAbsolute = 0x20, JmpAbsolute = 0x4c, Beq = 0xf0,
};
// Encode interrupt masking.
constexpr Instruction<1> sei() { return {{Sei}}; }
// Encode interrupt enabling.
constexpr Instruction<1> cli() { return {{Cli}}; }
// Encode interrupt return.
constexpr Instruction<1> rti() { return {{Rti}}; }
// Encode subroutine return.
constexpr Instruction<1> rts() { return {{Rts}}; }
// Encode stack-pointer transfer from X.
constexpr Instruction<1> txs() { return {{Txs}}; }
// Encode an accumulator immediate load.
constexpr Instruction<2> lda_imm(uint8_t value) { return {{LdaImmediate, value}}; }
// Encode an X-register immediate load.
constexpr Instruction<2> ldx_imm(uint8_t value) { return {{LdxImmediate, value}}; }
// Encode a checked zero-page read.
constexpr Instruction<2> lda_zp(econopet::CpuAddress address) { return {{LdaZeroPage, direct_address(address)}}; }
// Encode a checked zero-page store.
constexpr Instruction<2> sta_zp(econopet::CpuAddress address) { return {{StaZeroPage, direct_address(address)}}; }
// Encode an absolute read using little-endian address bytes.
constexpr Instruction<3> lda_abs(econopet::CpuAddress address) {
    return {{LdaAbsolute, static_cast<uint8_t>(address.value()), static_cast<uint8_t>(address.value() >> 8)}};
}
// Encode an absolute store using little-endian address bytes.
constexpr Instruction<3> sta_abs(econopet::CpuAddress address) {
    return {{StaAbsolute, static_cast<uint8_t>(address.value()), static_cast<uint8_t>(address.value() >> 8)}};
}
// Encode an absolute call using little-endian address bytes.
constexpr Instruction<3> jsr_abs(econopet::CpuAddress address) {
    return {{JsrAbsolute, static_cast<uint8_t>(address.value()), static_cast<uint8_t>(address.value() >> 8)}};
}
// Encode an absolute jump using little-endian address bytes.
constexpr Instruction<3> jmp_abs(econopet::CpuAddress address) {
    return {{JmpAbsolute, static_cast<uint8_t>(address.value()), static_cast<uint8_t>(address.value() >> 8)}};
}
// Encode a jump to an owned program label.
inline LabelInstruction<Mos6502> jmp_abs(const Label& label) { return {JmpAbsolute, label, Relocation::LittleEndian16}; }
// Encode a call to an owned program label.
inline LabelInstruction<Mos6502> jsr_abs(const Label& label) { return {JsrAbsolute, label, Relocation::LittleEndian16}; }
// Encode an equality branch to an owned program label.
inline LabelInstruction<Mos6502> beq(const Label& label) { return {Beq, label, Relocation::Relative8}; }
}

namespace mc6809 {
template<size_t Size> using Instruction = test_asm::Instruction<Mc6809, Size>;
using Label = test_asm::Label<Mc6809>;
enum Opcode : uint8_t {
    PageTwo = 0x10, LdsImmediate = 0xce, LdxImmediate = 0x8e, StxDirect = 0x9f,
    ClrIndexed = 0x6f, LdbImmediate = 0xc6, LddImmediate = 0xcc, LdaImmediate = 0x86,
    LdaExtended = 0xb6, JsrExtended = 0xbd, StaExtended = 0xb7, StbExtended = 0xf7,
    Bra = 0x20, OrccImmediate = 0x1a, Rts = 0x39,
};
enum IndexedMode : uint8_t { AtX = 0x84, OffsetMask = 0x1f };
// Encode hardware stack initialization with a big-endian immediate.
constexpr Instruction<4> lds_imm(uint16_t value) {
    return {{PageTwo, LdsImmediate, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)}};
}
// Encode a checked stack pointer.
constexpr Instruction<4> lds_imm(econopet::CpuAddress address) { return lds_imm(static_cast<uint16_t>(address.value())); }
// Encode condition-code flag setting.
constexpr Instruction<2> orcc_imm(uint8_t flags) { return {{OrccImmediate, flags}}; }
// Encode subroutine return.
constexpr Instruction<1> rts() { return {{Rts}}; }
// Encode an X-register immediate.
constexpr Instruction<3> ldx_imm(uint16_t value) {
    return {{LdxImmediate, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)}};
}
// Encode a checked CPU pointer in X.
constexpr Instruction<3> ldx_imm(econopet::CpuAddress address) { return ldx_imm(static_cast<uint16_t>(address.value())); }
// Encode a direct-page X store, with a checked offset byte.
constexpr Instruction<2> stx_dp(econopet::CpuAddress address) { return {{StxDirect, direct_address(address)}}; }
// Encode the ordinary ,X indexed clear.
constexpr Instruction<2> clr_indexed() { return {{ClrIndexed, AtX}}; }
// Deliberately emit an unchecked indexed postbyte requiring caller-supplied trailing bytes.
constexpr Instruction<2> raw_clr_indexed_postbyte(uint8_t postbyte) { return {{ClrIndexed, postbyte}}; }
// Encode only a signed five-bit displacement from X.
template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
constexpr Instruction<2> clr_indexed_offset(Integer offset) {
    static_assert(sizeof(Integer) <= sizeof(uint64_t));
    constexpr int Minimum = -16, Maximum = 15;
    if constexpr (std::is_signed_v<Integer>) {
        if (offset < Minimum)
            throw std::out_of_range("6809 clr indexed: X offset=" + std::to_string(offset) + " is outside -16..15");
    }
    if (offset > Maximum)
        throw std::out_of_range("6809 clr indexed: X offset=" + std::to_string(offset) + " is outside -16..15");
    return {{ClrIndexed, static_cast<uint8_t>(static_cast<uint8_t>(offset) & OffsetMask)}};
}
// Encode a B-register byte immediate.
constexpr Instruction<2> ldb_imm(uint8_t value) { return {{LdbImmediate, value}}; }
// Encode a D-register word immediate.
constexpr Instruction<3> ldd_imm(uint16_t value) {
    return {{LddImmediate, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)}};
}
// Encode an A-register byte immediate.
constexpr Instruction<2> ldaa_imm(uint8_t value) { return {{LdaImmediate, value}}; }
// Encode an extended A-register read without implicit direct-page shortening.
constexpr Instruction<3> lda_abs(econopet::CpuAddress address) {
    return {{LdaExtended, static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value())}};
}
// Encode a big-endian extended call.
constexpr Instruction<3> jsr_abs(econopet::CpuAddress address) {
    return {{JsrExtended, static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value())}};
}
// Encode a big-endian extended A store.
constexpr Instruction<3> sta_abs(econopet::CpuAddress address) {
    return {{StaExtended, static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value())}};
}
// Encode a big-endian extended B store.
constexpr Instruction<3> stb_abs(econopet::CpuAddress address) {
    return {{StbExtended, static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value())}};
}
// Encode a checked signed eight-bit branch displacement.
template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
constexpr Instruction<2> bra(Integer offset) {
    static_assert(sizeof(Integer) <= sizeof(uint64_t));
    if constexpr (std::is_signed_v<Integer>) {
        if (offset < std::numeric_limits<int8_t>::min())
            throw std::out_of_range("6809 bra: displacement=" + std::to_string(offset) + " is outside -128..127");
    }
    if (offset > std::numeric_limits<int8_t>::max())
        throw std::out_of_range("6809 bra: displacement=" + std::to_string(offset) + " is outside -128..127");
    return {{Bra, static_cast<uint8_t>(offset)}};
}
// Encode a relative branch to an owned label.
inline LabelInstruction<Mc6809> bra(const Label& label) { return {Bra, label, Relocation::Relative8}; }
// Encode an extended call to an owned label.
inline LabelInstruction<Mc6809> jsr_abs(const Label& label) { return {JsrExtended, label, Relocation::BigEndian16}; }
}

// Shared data instructions preserve ownership and emit real CPU operations.
template<class Derived>
class OwnedInstructions {
public:
    // Emit an absolute/extended store after checking layout identity.
    Derived& sta(const MemoryByte& byte) {
        auto& p = static_cast<Derived&>(*this);
        p.require_data(byte);
        return p.sta(byte.address());
    }
    // Emit an absolute/extended read after checking layout identity.
    Derived& lda(const MemoryByte& byte) {
        auto& p = static_cast<Derived&>(*this);
        p.require_data(byte);
        return p.lda(byte.address());
    }
    // Emit a complete word as two loads/stores, clobbering A and load-related flags.
    Derived& store(const MemoryWord& word, ImmediateWord value) {
        auto& p = static_cast<Derived&>(*this);
        p.require_data(word.byte(0));
        p.require_data(word.byte(1));
        p.require_space(BytesPerStore * 2);
        const auto low = static_cast<uint8_t>(value.value), high = static_cast<uint8_t>(value.value >> 8);
        p.lda(imm(word.order() == ByteOrder::LittleEndian ? low : high)).sta(word.byte(0));
        return p.lda(imm(word.order() == ByteOrder::LittleEndian ? high : low)).sta(word.byte(1));
    }
    // Emit a checked CPU pointer in the word's declared storage order.
    Derived& store(const MemoryWord& word, econopet::CpuAddress pointer) { return store(word, imm16(pointer)); }
    // Emit a complete buffer after validating length, ownership and code capacity.
    Derived& store(const MemoryBuffer& buffer, econopet::ByteView bytes) {
        auto& p = static_cast<Derived&>(*this);
        p.require_data(buffer.byte(0));
        p.require_data(buffer.byte(buffer.size() - 1));
        if (bytes.size() != buffer.size())
            throw p.template data_failure<std::out_of_range>("program store", "buffer length mismatch");
        p.require_space(BytesPerStore * buffer.size());
        for (size_t i = 0; i < bytes.size(); ++i) p.lda(imm(bytes[i])).sta(buffer.byte(i));
        return p;
    }
protected:
    // Delegate bounds and ownership enforcement to the checked program.
    template<class Encoded> Derived& append(const Encoded& instruction) {
        auto& receiver = static_cast<Derived&>(*this);
        receiver.emit(instruction);
        return receiver;
    }
private:
    static constexpr size_t BytesPerStore = 5;
};

template<class Derived>
class FluentInstructions<Mos6502, Derived> : public OwnedInstructions<Derived> {
public:
    using OwnedInstructions<Derived>::lda;
    using OwnedInstructions<Derived>::sta;
private:
    using OwnedInstructions<Derived>::append;
public:
    // Emit explicit zero-page pointer stores after validating both zero-page bytes.
    Derived& store_zp(const MemoryWord& word, econopet::CpuAddress pointer) {
        auto& p = static_cast<Derived&>(*this);
        p.require_data(word.byte(0));
        p.require_data(word.byte(1));
        const auto a = zp(word.byte(0).address()), b = zp(word.byte(1).address());
        constexpr size_t ZeroPageWordStoreSize = 8;
        p.require_space(ZeroPageWordStoreSize);
        const auto low = static_cast<uint8_t>(pointer.value()), high = static_cast<uint8_t>(pointer.value() >> 8);
        p.lda(imm(word.order() == ByteOrder::LittleEndian ? low : high)).sta(a);
        return p.lda(imm(word.order() == ByteOrder::LittleEndian ? high : low)).sta(b);
    }
    // Emit interrupt masking.
    Derived& sei() { return append(mos6502::sei()); }
    // Emit interrupt enabling.
    Derived& cli() { return append(mos6502::cli()); }
    // Emit stack-pointer transfer.
    Derived& txs() { return append(mos6502::txs()); }
    // Emit interrupt return.
    Derived& rti() { return append(mos6502::rti()); }
    // Emit subroutine return.
    Derived& rts() { return append(mos6502::rts()); }
    // Emit a typed byte immediate load.
    Derived& lda(ImmediateByte value) { return append(mos6502::lda_imm(value.value)); }
    // Emit an explicitly selected zero-page load.
    Derived& lda(ZeroPage address) { return append(mos6502::lda_zp(address.address())); }
    // Emit an absolute load without shortening low addresses.
    Derived& lda(econopet::CpuAddress address) { return append(mos6502::lda_abs(address)); }
    // Emit a typed X-register immediate load.
    Derived& ldx(ImmediateByte value) { return append(mos6502::ldx_imm(value.value)); }
    // Emit an explicitly selected zero-page store.
    Derived& sta(ZeroPage address) { return append(mos6502::sta_zp(address.address())); }
    // Emit an absolute store.
    Derived& sta(econopet::CpuAddress address) { return append(mos6502::sta_abs(address)); }
    // Emit an absolute subroutine call.
    Derived& jsr(econopet::CpuAddress address) { return append(mos6502::jsr_abs(address)); }
    // Emit an owned-label subroutine call.
    Derived& jsr(const Label<Mos6502>& label) { return append(mos6502::jsr_abs(label)); }
    // Emit an absolute jump.
    Derived& jmp(econopet::CpuAddress address) { return append(mos6502::jmp_abs(address)); }
    // Emit an owned-label jump.
    Derived& jmp(const Label<Mos6502>& label) { return append(mos6502::jmp_abs(label)); }
    // Emit an owned-label equality branch.
    Derived& beq(const Label<Mos6502>& label) { return append(mos6502::beq(label)); }
};

template<class Derived>
class FluentInstructions<Mc6809, Derived> : public OwnedInstructions<Derived> {
public:
    using OwnedInstructions<Derived>::lda;
    using OwnedInstructions<Derived>::sta;
private:
    using OwnedInstructions<Derived>::append;
public:
    // Emit an A-register byte immediate.
    Derived& lda(ImmediateByte value) { return append(mc6809::ldaa_imm(value.value)); }
    // Emit an extended A-register load.
    Derived& lda(econopet::CpuAddress address) { return append(mc6809::lda_abs(address)); }
    // Emit a B-register byte immediate.
    Derived& ldb(ImmediateByte value) { return append(mc6809::ldb_imm(value.value)); }
    // Emit a D-register word immediate.
    Derived& ldd(ImmediateWord value) { return append(mc6809::ldd_imm(value.value)); }
    // Emit an X-register word immediate.
    Derived& ldx(ImmediateWord value) { return append(mc6809::ldx_imm(value.value)); }
    // Emit hardware stack initialization.
    Derived& lds(ImmediateWord value) { return append(mc6809::lds_imm(value.value)); }
    // Emit condition-code flag setting.
    Derived& orcc(ImmediateByte value) { return append(mc6809::orcc_imm(value.value)); }
    // Emit an extended A store.
    Derived& sta(econopet::CpuAddress address) { return append(mc6809::sta_abs(address)); }
    // Emit an extended B store.
    Derived& stb(econopet::CpuAddress address) { return append(mc6809::stb_abs(address)); }
    // Emit an explicitly selected direct-page X store.
    Derived& stx(DirectPage offset) { return append(mc6809::stx_dp(offset.offset())); }
    // Emit an extended subroutine call.
    Derived& jsr(econopet::CpuAddress address) { return append(mc6809::jsr_abs(address)); }
    // Emit an owned-label extended call.
    Derived& jsr(const Label<Mc6809>& label) { return append(mc6809::jsr_abs(label)); }
    // Emit an owned-label relative branch.
    Derived& bra(const Label<Mc6809>& label) { return append(mc6809::bra(label)); }
    // Emit a checked literal branch displacement, distinct from a label.
    template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
    Derived& bra_offset(Integer offset) { return append(mc6809::bra(offset)); }
    // Emit subroutine return.
    Derived& rts() { return append(mc6809::rts()); }
    // Emit the ordinary ,X indexed clear.
    Derived& clr_x() { return append(mc6809::clr_indexed()); }
    // Emit a checked signed five-bit X displacement.
    template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
    Derived& clr_x_offset(Integer offset) { return append(mc6809::clr_indexed_offset(offset)); }
    // Emit an explicitly unchecked indexed postbyte.
    Derived& raw_clr_indexed_postbyte(uint8_t postbyte) { return append(mc6809::raw_clr_indexed_postbyte(postbyte)); }
};

// Buffer CPU-specific code and resolve all references before writing SRAM.
template<class Cpu, class Memory>
class Program : public FluentInstructions<Cpu, Program<Cpu, Memory>> {
    static_assert(std::is_same_v<Cpu, Mos6502> || std::is_same_v<Cpu, Mc6809>);
    template<class> friend class OwnedInstructions;
    template<class, class> friend class FluentInstructions;
    template<class> friend class Scenario;
    template<class, class> friend class Assembly;
public:
    using CpuType = Cpu;
    // Borrow a persistent layout and assemble only inside its owned code region.
    Program(Scenario<Memory>& scenario, const Region& region) : scenario_(scenario), region_(region) {
        scenario_.require(region_, RegionKind::Code);
    }
    Program(Scenario<Memory>&&, const Region&) = delete;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    Program(Program&&) = default;
    Program& operator=(Program&&) = delete;

    // Borrow an existing ordinary-data reservation from the same layout.
    MemoryView<Memory> lower_bank(const std::string& name) {
        require_live();
        return scenario_.lower_bank(scenario_.region(name));
    }
    // Append an instruction for this CPU without changing backing memory.
    template<size_t Size>
    void emit(const Instruction<Cpu, Size>& instruction) { raw_bytes(instruction.bytes); }
    // Append a checked label reference for later CPU-specific relocation.
    void emit(const LabelInstruction<Cpu>& instruction) {
        require_label(instruction.target);
        size_t size;
        switch (instruction.relocation) {
        case Relocation::Relative8: size = RelativeInstructionSize; break;
        case Relocation::LittleEndian16: case Relocation::BigEndian16: size = AbsoluteInstructionSize; break;
        default: throw failure<std::invalid_argument>("emit", "unknown fixture relocation");
        }
        if ((std::is_same_v<Cpu, Mos6502> && instruction.relocation == Relocation::BigEndian16)
            || (std::is_same_v<Cpu, Mc6809> && instruction.relocation == Relocation::LittleEndian16))
            throw failure<std::invalid_argument>("emit", "relocation byte order does not match CPU");
        require_space(size);
        fixups_.push_back({bytes_.size(), instruction});
        bytes_.push_back(instruction.opcode);
        bytes_.resize(bytes_.size() + size - 1);
    }
    // Deliberately append unvalidated opcodes/data while retaining region bounds.
    void raw_bytes(econopet::ByteView bytes) {
        require_space(bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i) bytes_.push_back(bytes[i]);
    }
    // Create a named forward reference without assigning an address.
    Label<Cpu> label(std::string name) {
        require_live();
        if (name.empty()) throw failure<std::invalid_argument>("label", "fixture label needs a name");
        for (const auto& state : labels_)
            if (state->name == name)
                throw failure<std::invalid_argument>("label", "duplicate fixture label: " + name);
        auto state = std::make_shared<typename Label<Cpu>::State>();
        state->name = std::move(name);
        labels_.push_back(state);
        return Label<Cpu>(std::move(state));
    }
    // Bind an owned label once at the next instruction address.
    void bind(const Label<Cpu>& label) {
        require_label(label);
        if (label.state_->address)
            throw failure<std::logic_error>("bind", "fixture label already bound: " + label.state_->name);
        label.state_->address = address();
    }
    // Create and bind a label at the current cursor.
    Label<Cpu> mark(std::string name) {
        const auto cursor = address();
        auto target = label(std::move(name));
        target.state_->address = cursor;
        return target;
    }
    // Return an owned label address, rejecting unresolved or foreign targets.
    econopet::CpuAddress address(const Label<Cpu>& label) const {
        require_label(label);
        if (!label.state_->address)
            throw failure<std::logic_error>("label address", "unresolved fixture label: " + label.state_->name);
        return *label.state_->address;
    }
    // Validate every label and branch before installing bytes, without clocks.
    void install() {
        const auto bytes = resolved_bytes();
        scenario_.write(region_, 0, bytes);
    }
    // Append a private completion store and permanent park loop atomically.
    void finish(const CompletionMarker& marker) {
        require_live();
        scenario_.require(marker.region_, RegionKind::Mailbox);
        if (completion_) throw failure<std::logic_error>("finish", "program already completed");
        if constexpr (std::is_same_v<Cpu, Mos6502>) {
            constexpr size_t TerminalSize = 8;
            require_space(TerminalSize);
            const auto park = region_.at(bytes_.size() + 5);
            emit(mos6502::lda_imm(marker.completed()));
            emit(mos6502::sta_abs(marker.address()));
            emit(mos6502::jmp_abs(park));
        } else {
            constexpr size_t TerminalSize = 7;
            require_space(TerminalSize);
            emit(mc6809::ldaa_imm(marker.completed()));
            emit(mc6809::sta_abs(marker.address()));
            emit(mc6809::bra(-2));
        }
        completion_ = marker;
    }
    // Return the first instruction address, independently of the cursor.
    econopet::CpuAddress entry() const { require_live(); return region_.start(); }
    // Return the next address, never the one-past address of a full region.
    econopet::CpuAddress address() const {
        require_live();
        if (bytes_.size() >= region_.size())
            throw failure<std::out_of_range>("program address", "cursor is past the reserved code region");
        return region_.at(bytes_.size());
    }
    // Return emitted size even when the code reservation is exactly full.
    size_t size() const { require_live(); return bytes_.size(); }

private:
    // Check operand identity before shared fluent data instructions are emitted.
    void require_data(const MemoryByte& byte) const { require_live(); scenario_.require(byte); }
    // Include code location and backing evidence in shared instruction diagnostics.
    template<class Exception>
    Exception data_failure(const std::string& operation, const std::string& detail) const {
        return failure<Exception>(operation, detail);
    }
    static constexpr size_t RelativeInstructionSize = 2;
    static constexpr size_t AbsoluteInstructionSize = 3;
    static constexpr int64_t BranchMinimum = std::numeric_limits<int8_t>::min();
    static constexpr int64_t BranchMaximum = std::numeric_limits<int8_t>::max();
    // Keep code purpose, requested CPU and assembly cursor in every program failure.
    template<class Exception>
    Exception failure(const std::string& operation, const std::string& detail) const {
        return scenario_.template failure<Exception>(operation, detail
            + ", program_cpu=" + (std::is_same_v<Cpu, Mos6502> ? "soft6502" : "soft6809")
            + ", code_address=" + std::to_string(region_.start().value())
            + ", capacity=" + std::to_string(region_.size()) + ", cursor=" + std::to_string(bytes_.size()));
    }
    // A moved program or moved layout cannot continue authoring or installation.
    void require_live() const {
        if (!owner_) throw failure<std::logic_error>("program", "fixture program was moved from");
        scenario_.require(region_, RegionKind::Code);
    }
    // Check the whole instruction or compound store before changing the code buffer.
    void require_space(size_t size) const {
        require_live();
        if (completion_) throw failure<std::logic_error>("emit", "cannot append instructions after program.finish");
        if (size > region_.size() - bytes_.size())
            throw failure<std::out_of_range>("emit", "fixture program exceeds reserved code region"
                ", requested=" + std::to_string(size));
    }
    // Reject labels from another program, including moved-from label handles.
    void require_label(const Label<Cpu>& label) const {
        require_live();
        for (const auto& state : labels_)
            if (state == label.state_) return;
        throw failure<std::invalid_argument>("label", "fixture label belongs to another program");
    }
    // Resolve all fixups into a detached byte vector before any backing writes.
    std::vector<uint8_t> resolved_bytes() const {
        require_live();
        for (const auto& state : labels_)
            if (!state->address)
                throw failure<std::logic_error>("resolve", "unresolved fixture label: " + state->name);
        auto resolved = bytes_;
        for (const auto& fixup : fixups_) {
            const auto target = address(fixup.instruction.target).value();
            const auto operand = fixup.offset + 1;
            switch (fixup.instruction.relocation) {
            case Relocation::Relative8: {
                const auto next_pc = (region_.start().value() + fixup.offset + RelativeInstructionSize)
                    % econopet::CpuAddressCapacity;
                const auto difference = (target + econopet::CpuAddressCapacity - next_pc)
                    % econopet::CpuAddressCapacity;
                const int64_t displacement = difference < econopet::CpuAddressCapacity / 2
                    ? static_cast<int64_t>(difference)
                    : static_cast<int64_t>(difference) - static_cast<int64_t>(econopet::CpuAddressCapacity);
                if (displacement < BranchMinimum || displacement > BranchMaximum)
                    throw failure<std::out_of_range>("resolve", "fixture branch displacement is outside -128..127"
                        ", target=" + std::to_string(target) + ", displacement=" + std::to_string(displacement));
                resolved[operand] = static_cast<uint8_t>(displacement);
                break;
            }
            case Relocation::LittleEndian16:
                resolved[operand] = static_cast<uint8_t>(target);
                resolved[operand + 1] = static_cast<uint8_t>(target >> 8);
                break;
            case Relocation::BigEndian16:
                resolved[operand] = static_cast<uint8_t>(target >> 8);
                resolved[operand + 1] = static_cast<uint8_t>(target);
                break;
            }
        }
        return resolved;
    }
    struct Fixup { size_t offset; LabelInstruction<Cpu> instruction; };
    Scenario<Memory>& scenario_;
    Region region_;
    std::shared_ptr<const int> owner_ = std::make_shared<const int>(0);
    std::vector<uint8_t> bytes_;
    std::vector<std::shared_ptr<typename Label<Cpu>::State>> labels_;
    std::vector<Fixup> fixups_;
    std::optional<CompletionMarker> completion_;
};


namespace mos6502 {
// Checked explicit 6502 program with fluent instruction authoring.
template<class Memory> using Program = test_asm::Program<Mos6502, Memory>;
}
namespace mc6809 {
// Checked explicit 6809 program with fluent instruction authoring.
template<class Memory> using Program = test_asm::Program<Mc6809, Memory>;
}

} // namespace test_asm
