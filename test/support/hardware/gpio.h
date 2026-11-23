// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Match the Pico board's status LED for host tests.
#define PICO_DEFAULT_LED_PIN 25

// Match the Pico SDK GPIO output direction.
#define GPIO_OUT 1
#define GPIO_IN 0

#ifdef __cplusplus
extern "C" {
#endif

// Configure GPIO through a fixture override, otherwise fail explicitly.
void gpio_init(unsigned int gpio);
// Set GPIO direction through a fixture override, otherwise fail explicitly.
void gpio_set_dir(unsigned int gpio, bool output);
// Configure a pull-up through a fixture override, otherwise fail explicitly.
void gpio_pull_up(unsigned int gpio);
// Sample a fixture-owned input, otherwise fail explicitly.
bool gpio_get(unsigned int gpio);

// Drive a fixture-owned output, otherwise fail explicitly.
void gpio_put(unsigned int gpio, bool value);

#ifdef __cplusplus
}
#endif
