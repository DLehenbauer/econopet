// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "pch.h"

void checksum_add(const uint8_t* buffer, size_t length, uint8_t* checksum);
uint8_t checksum_fix(uint8_t current_byte, int actual_sum, int expected_sum);
