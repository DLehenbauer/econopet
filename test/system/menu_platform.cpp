// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "firmware_transport.h"

extern "C" {
#include "display/dvi/dvi.h"
#include "driver.h"
#include "input/keyboard_core.h"
}

uint8_t pet_key_matrix[ECONOPET_KBD_COL_COUNT];
uint8_t usb_key_matrix[ECONOPET_KBD_COL_COUNT];

namespace {
constexpr uint64_t CyclesPerMicrosecond = 64;
} // namespace

/** Derive scanner dwell and timeout clocks from simulated bus progress, not wall time. */
extern "C" uint64_t __wrap_time_us_64(void) { return firmware_system().time().value() / CyclesPerMicrosecond; }

/** Advance the board and scheduled external peers during firmware settling delays. */
extern "C" void __wrap_sleep_us(uint64_t us) {
    firmware_system().tick(econopet::Cycles{us * CyclesPerMicrosecond});
}
