// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <check.h>

// Resolves an independently checked D64 fixture coordinate.
uint32_t diskimage_test_d64_offset(unsigned int track, unsigned int sector);
// Builds an ordinary two-sector BASIC PRG fixture.
uint8_t* diskimage_test_make_d64(void);
// Builds an ordinary four-byte BASIC PRG fixture.
uint8_t* diskimage_test_make_d80(void);
// Exercises raw format recognition and geometry without DOS algorithms.
Suite* diskimage_suite(void);
