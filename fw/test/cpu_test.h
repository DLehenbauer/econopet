// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <check.h>

// Fork each initialization scenario to preserve the production one-time lifecycle.
Suite* cpu_suite(void);
