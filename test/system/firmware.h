// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "d64.h"
#include "ieee/transactions.h"
#include "system.h"

extern "C" {
#include "ieee/ieee_drive.h"
#include "test_support.h"
}

// RX observations share the wire record shape, with EOI explicitly unobserved.
using IeeeRxRecord = ieee_test::Record;
class FirmwareRxTrace;

// Owns the process-global firmware binding, independently of any test framework.
// Declare after the board. Concurrent or nested sessions are unsupported.
// Mutating operations require an idle bus, outside observation/input-edit/stimulus callbacks.
class FirmwareSession {
public:
    // Receive the original teardown exception of any type and return without throwing.
    using TeardownErrorHandler = void (*)(std::exception_ptr);

    // Bind physical SPI and initialize fresh production drive state.
    explicit FirmwareSession(System& system, TeardownErrorHandler teardown_error = nullptr);
    // Close the session. An unhandled teardown failure is fatal, never ignored.
    ~FirmwareSession();
    FirmwareSession(const FirmwareSession&) = delete;
    FirmwareSession& operator=(const FirmwareSession&) = delete;
    // Unmount and release all owned globals, propagating any teardown error.
    void close();
    // Install a writable fixture under an unregistered shared path in drive slot zero.
    void mount(const std::vector<uint8_t>& image, const char* name = "test.d64");
    // Mount an owned checked fixture, copying bytes before the temporary expires.
    void mount(const disk_fixture::D64& image, const char* name = "test.d64") {
        mount(image.bytes(), name);
    }
    // Run one cooperative drive service pass through real FPGA SPI pins.
    void service();
    // Service this board while the session lives, rejecting closed or expired owners.
    RunConfiguration::Service service_adapter();
    // Own RX storage and registration, recording absolute production status/RX reads.
    FirmwareRxTrace trace_rx();

private:
    std::shared_ptr<bool> active_ = std::make_shared<bool>(false);
    System& board_;
    std::vector<std::string> fixture_paths_;
    TeardownErrorHandler teardown_error_;
    test_observation::TraceSource<IeeeRxRecord> rx_source_;
};

// Records adjacent absolute status/RX reads, with no intervening board cycles.
// This semantic trace does not decode arbitrary pipelined or burst traffic.
// RX contains byte/ATN only. Use ieee_test::WireTrace for EOI evidence.
class FirmwareRxTrace : public test_observation::ScopedRecorder<IeeeRxRecord> {
public:
    FirmwareRxTrace(const FirmwareRxTrace&) = delete;
    FirmwareRxTrace& operator=(const FirmwareRxTrace&) = delete;
    FirmwareRxTrace(FirmwareRxTrace&&) noexcept = default;
    FirmwareRxTrace& operator=(FirmwareRxTrace&&) noexcept = default;
private:
    friend class FirmwareSession;
    // Register with this session only, allowing independent simultaneous recorders.
    FirmwareRxTrace(System& board, test_observation::TraceSource<IeeeRxRecord>& source);
};
