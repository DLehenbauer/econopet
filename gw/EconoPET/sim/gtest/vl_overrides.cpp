// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

//
// Verilator runtime overrides for the GoogleTest harness.
//
// Defining VL_USER_FATAL (see the target's compile definitions in CMakeLists.txt)
// suppresses Verilator's default 'vl_fatal', which prints and calls std::exit().
// Exiting the process would turn a testbench assertion failure into a crash rather
// than a clean GoogleTest failure, so we provide our own implementation that:
//
//   1. Records the '$fatal' message so the harness can report it via FAIL(), and
//   2. Signals the current VerilatedContext to stop so the eval loop unwinds
//      cleanly (rather than throwing across Verilator's --timing coroutines).
//
// 'vl_finish' and 'vl_stop' keep their default behavior (VL_USER_FINISH / VL_USER_STOP
// are intentionally NOT defined), so a normal '$finish' still sets gotFinish().
//

#include <string>

#include "verilated.h"

#include "sim_harness.h"

namespace sim_harness {
namespace {
thread_local bool g_had_fatal = false;
thread_local std::string g_fatal_message;
}  // namespace

void record_fatal(const std::string& msg) {
    if (!g_had_fatal) {
        g_had_fatal = true;
        g_fatal_message = msg;
    }
}

bool had_fatal() { return g_had_fatal; }

const std::string& fatal_message() { return g_fatal_message; }

void reset_fatal() {
    g_had_fatal = false;
    g_fatal_message.clear();
}
}  // namespace sim_harness

void vl_fatal(const char* filename, int lineno, const char* hier, const char* msg) {
    std::string full = std::string(filename ? filename : "") + ":" +
                       std::to_string(lineno);
    if (hier && hier[0]) {
        full += std::string(" ") + hier;
    }
    full += ": ";
    full += (msg ? msg : "");

    sim_harness::record_fatal(full);

    // Do not exit: ask the eval loop to stop cleanly.
    if (VerilatedContext* ctx = Verilated::threadContextp()) {
        ctx->gotFinish(true);
    }
}
