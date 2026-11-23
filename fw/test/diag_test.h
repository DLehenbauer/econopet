// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <check.h>

/** Bounded diagnostic runner, display, CLI, and memory-plan tests. */
Suite* diag_suite(void);
/** Process-isolated terminal-loop and declaration-validation tests. */
Suite* diag_fatal_suite(void);
