// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <check.h>

Suite* ieee_drive_suite(void);

// Returns path-validation tests that require a forked runner for fatal errors.
Suite* ieee_drive_fatal_suite(void);