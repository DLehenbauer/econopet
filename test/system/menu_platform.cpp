// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <stdexcept>

#include "firmware_transport.h"

extern "C" {
#include "display/dvi/dvi.h"
#include "driver.h"
#include "input/keyboard_core.h"
}

uint8_t pet_key_matrix[KEY_COL_COUNT];
uint8_t usb_key_matrix[KEY_COL_COUNT];

namespace {
bool renderer_owned;
constexpr uint64_t CyclesPerMicrosecond = 64;
} // namespace

/** Derive scanner dwell and timeout clocks from simulated bus progress, not wall time. */
extern "C" uint64_t __wrap_time_us_64(void) { return firmware_system().time().value() / CyclesPerMicrosecond; }

/** Model exclusive publication ownership without pretending to simulate DVI scanout. */
extern "C" bool video_transition_begin(void) {
    if (renderer_owned) {
        throw std::logic_error("nested host renderer ownership");
    }
    renderer_owned = true;
    return true;
}

/** Release the host consumer's publication ownership after shared state is stable. */
extern "C" bool video_transition_end(void) {
    if (!renderer_owned) {
        throw std::logic_error("unowned host renderer publication");
    }
    renderer_owned = false;
    return true;
}

/** Require menu attribute source changes to occur while the host renderer is excluded. */
extern "C" void video_set_menu_attributes(bool enabled) {
    if (enabled && !renderer_owned) {
        throw std::logic_error("unowned host menu attribute selection");
    }
}
