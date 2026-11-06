// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Match the Pico board's status LED for host tests.
#define PICO_DEFAULT_LED_PIN 25

// Record GPIO writes in the SD activity tests.
void gpio_put(unsigned int gpio, bool value);
