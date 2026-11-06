// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Shared filesystem fixture controls used by browser and menu integration tests.
extern char directory_root[64];
extern bool fail_readdir;
extern bool fail_next_readdir;
extern bool unknown_entry_types;
extern unsigned int directory_scans, metadata_reads;

#include <check.h>

// Creates the standalone bounded filesystem browser suite.
Suite* menu_browser_suite(void);

// Creates the fork-isolated browser invariant suite.
Suite* menu_browser_fatal_suite(void);
