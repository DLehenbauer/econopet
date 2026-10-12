// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include "framework/asm.h"
#include "registers.h"

// Shared physical line states and RAM harness layout, not ROM workspace.
namespace ieee_test {
using econopet::io::PiaC2High;
using econopet::io::PiaC2Low;
inline constexpr uint8_t BusReleased = 0xff;
inline constexpr uint8_t EoiInput = 0x40; // PIA1 PA6, not a VIA port-B line.
inline constexpr econopet::CpuAddress HarnessStart{0x0300};
inline constexpr econopet::Cycles ByteBudget = 500000;
} // namespace ieee_test
