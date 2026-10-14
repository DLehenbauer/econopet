// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include "firmware.h"
#include "framework/system_test.h"

// Keep local test sessions' unwind diagnostics inside the current GoogleTest case.
class FirmwareTestSession : public FirmwareSession {
public:
    // Bind the independent session with the test framework's failure reporter.
    explicit FirmwareTestSession(System& system) : FirmwareSession(system, report_teardown_error) {}

private:
    friend class FirmwareTest;
    // Preserve cleanup errors as test failures without terminating the runner.
    static void report_teardown_error(std::exception_ptr error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& detail) {
            ADD_FAILURE() << "firmware teardown failed: " << detail.what();
        } catch (...) {
            ADD_FAILURE() << "firmware teardown failed: non-standard exception";
        }
    }
};

// Adds firmware ownership and GoogleTest teardown diagnostics to a fresh board.
class FirmwareTest : public SystemTest {
protected:
    // Report cleanup failure before the board and session are destroyed.
    void TearDown() override {
        try {
            firmware.close();
        } catch (...) {
            FirmwareTestSession::report_teardown_error(std::current_exception());
        }
        SystemTest::TearDown();
    }
    FirmwareTestSession firmware{system};
};
