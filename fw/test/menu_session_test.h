// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once
#include <check.h>

/** Exercise bounded snapshot, readable PIA restoration, and scanner ownership. */
Suite* menu_session_suite(void);

/** Exercise menu session paths that require one-time CPU startup isolation. */
Suite* menu_session_fatal_suite(void);
