// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <check.h>

Suite* tui_suite(void);
// Returns the TUI suite for process-isolated fatal-path tests.
Suite* tui_fatal_suite(void);
