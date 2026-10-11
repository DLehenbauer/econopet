// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include "framework/scheduler.h"
#include "system.h"

// Explicit absolute-time scheduler for low-level timing tests.
using Scheduler = DeterministicScheduler;
