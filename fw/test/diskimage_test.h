// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <check.h>

uint32_t diskimage_test_d64_offset(unsigned int track, unsigned int sector);
uint8_t* diskimage_test_make_d64(void);
uint8_t* diskimage_test_make_d80(void);

Suite* diskimage_suite(void);
