// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <check.h>

Suite *log_suite(void);

// Register invalid log-level regressions for forked execution.
Suite* log_fatal_suite(void);
