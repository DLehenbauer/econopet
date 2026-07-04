// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

//
// Generic GoogleTest driver for the self-checking SystemVerilog testbenches.
//
// Each legacy '*_tb.sv' testbench is a self-contained, self-checking module: it
// generates its own clock, runs a 'task run', asserts with '$fatal', and calls
// '$finish' when done. Verilator (with '--timing') runs that timed SystemVerilog
// autonomously, so the C++ side just has to pump 'eval()' until the model reaches
// '$finish' (pass) or a '$fatal' fires (fail).
//
// Pass/fail detection:
//   - '$finish'  -> VerilatedContext::gotFinish() becomes true (clean pass).
//   - '$fatal'   -> our vl_fatal override (see vl_overrides.cpp) records the
//                   message and asks the eval loop to stop, so the failure is
//                   reported as a GoogleTest FAIL instead of aborting the process.
//

#ifndef SIM_HARNESS_H
#define SIM_HARNESS_H

#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "verilated.h"

namespace sim_harness {

// Records the message from the most recent '$fatal' (set by vl_fatal).
void record_fatal(const std::string& msg);
bool had_fatal();
const std::string& fatal_message();
void reset_fatal();

// Runs a Verilated testbench model to completion.
//
// 'max_time' is a safety net (in simulation time-precision units) that prevents a
// buggy testbench from looping forever. Normal completion is driven by the
// testbench's own '$finish'. CTest's per-test TIMEOUT is the outer backstop.
template <typename Model>
void run(std::uint64_t max_time = 5'000'000'000ull) {
    reset_fatal();

    auto ctx = std::make_unique<VerilatedContext>();
    ctx->assertOn(true);

    auto dut = std::make_unique<Model>(ctx.get());

    while (!ctx->gotFinish() && ctx->time() < max_time) {
        dut->eval();
        if (!dut->eventsPending()) break;
        ctx->timeInc(dut->nextTimeSlot() - ctx->time());
    }

    dut->final();

    if (had_fatal()) {
        FAIL() << fatal_message();
    }
    ASSERT_TRUE(ctx->gotFinish())
        << "Testbench did not reach $finish (possible hang or timeout at "
        << ctx->time() << " time units)";
}

}  // namespace sim_harness

#endif  // SIM_HARNESS_H
