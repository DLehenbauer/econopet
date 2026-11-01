// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include "firmware.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <utility>

#include "firmware_transport.h"

extern "C" {
#include "sd/sd.h"
}
namespace {
System* active_top_system;

test_observation::TraceSource<IeeeRxRecord>* firmware_rx_source;
FirmwareRxSample firmware_rx_status;
econopet::CycleTime firmware_rx_sampled_at{0};

} // namespace

// Require an owning session before crossing the physical SPI boundary.
System& firmware_system() {
    if (!active_top_system)
        throw std::logic_error("firmware SPI without an active production-top system");
    active_top_system->require_spi_idle();
    return *active_top_system;
}

// Clear semantic pairing before issuing another physical SPI operation.
FirmwareRxSample firmware_take_read_status() {
    const auto& board = firmware_system();
    const auto pending = std::exchange(firmware_rx_status, {});
    return board.time() == firmware_rx_sampled_at ? pending : FirmwareRxSample{};
}

// Record only an adjacent CTRL/STATUS/RX triple used by production servicing.
void firmware_record_read(uint32_t address, uint8_t value, FirmwareRxSample previous) {
    if (firmware_rx_source != nullptr && address == fpga::address(fpga::Register::IeeeControl).value()) {
        firmware_rx_status = {{}, fpga::IeeeControlStatus::from_bits(value)};
        firmware_rx_sampled_at = firmware_system().time();
    } else if (firmware_rx_source != nullptr && address == fpga::address(fpga::Register::IeeeStatus).value()) {
        firmware_rx_status = {fpga::IeeeStatus::from_bits(value), previous.status ? std::nullopt : previous.control};
        firmware_rx_sampled_at = firmware_system().time();
    } else if (firmware_rx_source != nullptr && address == fpga::address(fpga::Register::IeeeRx).value() &&
               previous.status && previous.status->contains(fpga::IeeeStatusBit::RxReady) && previous.control) {
        if (!previous.control->contains(fpga::IeeeControlStatusBit::RxAtnEnd) &&
            !previous.control->contains(fpga::IeeeControlStatusBit::RxTalkEvent))
            firmware_rx_source->emit(firmware_system().time(),
                                     {value, previous.status->contains(fpga::IeeeStatusBit::RxAtn),
                                      previous.control->contains(fpga::IeeeControlStatusBit::RxEoi)
                                          ? ieee_test::Eoi::Last
                                          : ieee_test::Eoi::More});
        firmware_rx_status = {};
    } else {
        firmware_rx_status = {};
    }
}

// Creates the sole firmware SPI binding and rolls it back if initialization fails.
FirmwareSession::FirmwareSession(System& system, TeardownErrorHandler teardown_error)
    : board_(system), teardown_error_(teardown_error) {
    // Reject nested owners before touching process-global firmware state.
    if (active_top_system)
        throw std::logic_error("only one firmware session per process is supported");
    board_.require_spi_idle();
    active_top_system = &system;
    firmware_rx_source = &rx_source_;
    firmware_rx_status = {};
    // Initialize the real drive, releasing the binding on exception unwind.
    try {
        ieee_drive_init();
        *active_ = true;
    } catch (...) {
        active_top_system = nullptr;
        firmware_rx_source = nullptr;
        throw;
    }
}

// Tears down firmware before releasing host fixtures and the board binding.
FirmwareSession::~FirmwareSession() {
    try {
        close();
    } catch (...) {
        if (teardown_error_) {
            teardown_error_(std::current_exception());
            // A rejected close may be retried only while its owner still exists.
            if (*active_) {
                std::fprintf(stderr, "firmware teardown cannot abandon an active session\n");
                std::terminate();
            }
        } else {
            try {
                throw;
            } catch (const std::exception& error) {
                std::fprintf(stderr, "firmware teardown failed: %s\n", error.what());
            } catch (...) {
                std::fprintf(stderr, "firmware teardown failed: non-standard exception\n");
            }
            std::terminate();
        }
    }
}

// Release session ownership even if production drive cleanup throws.
void FirmwareSession::close() {
    if (!*active_)
        return;
    board_.require_mutable_access();
    // Reject reentrant teardown before it closes files used by active firmware.
    if (board_.dispatching_stimulus_ || board_.spi_active_ || !board_.snapshot().spi_cs_ni ||
        board_.snapshot().spi_sck_i)
        throw board_.failure<std::logic_error>("firmware close", "requires an idle bus outside scheduled stimulus");
    std::exception_ptr error;
    bool unmounted = false;
    try {
        unmounted = ieee_drive_unmount_all();
    } catch (...) {
        error = std::current_exception();
        // A failed board cannot quiesce through SPI again. Retire native
        // borrowers while fixture storage is still alive, then rethrow below.
        ieee_drive_abort();
    }
    if (!unmounted && !error)
        throw board_.failure<std::runtime_error>("firmware close", "drive media are busy");
    // Remove all process-global observations and bindings owned by this session.
    for (const auto& path : fixture_paths_)
        test_unregister_file(path.c_str());
    fixture_paths_.clear();
    rx_source_.stop();
    firmware_rx_source = nullptr;
    firmware_rx_status = {};
    active_top_system = nullptr;
    *active_ = false;
    if (error)
        std::rethrow_exception(error);
}

// Registers and mounts the fixture in the firmware's first drive slot.
void FirmwareSession::mount(const std::vector<uint8_t>& image, const char* name, const unsigned int slot) {
    if (!*active_)
        throw board_.failure<std::logic_error>("firmware mount", "session is closed");
    board_.require_spi_idle();
    if (!name || !*name)
        throw board_.failure<std::invalid_argument>("firmware mount", "fixture name is empty");
    const auto prefix_length = std::strlen(sd_dir_prefix(SD_DIR_DISKS));
    if (std::strlen(name) >= SD_PATH_MAX - prefix_length)
        throw board_.failure<std::out_of_range>("firmware mount", "fixture path exceeds SD_PATH_MAX");
    if (image.empty())
        throw board_.failure<std::invalid_argument>("firmware mount", "image is empty");
    // Expose a writable fixture at the same path production firmware will open.
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, name);
    if (test_file_registered(path))
        throw board_.failure<std::invalid_argument>("firmware mount", "fixture name is already registered");
    fixture_paths_.emplace_back(path);
    test_register_binary_file(path, image.data(), image.size(), true);
    // Surface mount failure instead of leaving a success-shaped fixture.
    if (!ieee_drive_mount(slot, name)) {
        test_unregister_file(path);
        fixture_paths_.pop_back();
        throw board_.failure<std::runtime_error>("firmware mount", std::string("cannot mount ") + path);
    }
}

// Reject stale session objects before touching a new owner's global drive state.
void FirmwareSession::service() {
    if (!*active_)
        throw board_.failure<std::logic_error>("firmware service", "session is closed");
    board_.require_spi_idle();
    ieee_drive_task();
}

// Keep optional firmware integration out of the board-only run configuration.
RunConfiguration::Service FirmwareSession::service_adapter() {
    if (!*active_)
        throw board_.failure<std::logic_error>("firmware service adapter", "session is closed");
    return [owner = std::weak_ptr<const bool>(active_)](System& board, econopet::Cycles) {
        const auto active = owner.lock();
        if (!active || !*active)
            throw board.failure<std::logic_error>("firmware service", "adapter belongs to a closed or expired session");
        if (&board != active_top_system)
            throw board.failure<std::invalid_argument>("firmware service", "adapter belongs to another board");
        board.require_spi_idle();
        ieee_drive_task();
    };
}

// A recorder belongs to one session, even if it survives close and a later session.
FirmwareRxTrace FirmwareSession::trace_rx() {
    if (!*active_)
        throw board_.failure<std::logic_error>("firmware trace RX", "session is closed");
    return FirmwareRxTrace(firmware_system(), rx_source_);
}

// Observe every board boundary and tap every supported semantic RX read.
FirmwareRxTrace::FirmwareRxTrace(System& board, test_observation::TraceSource<IeeeRxRecord>& source)
    : ScopedRecorder(board) {
    source.subscribe(storage());
}
