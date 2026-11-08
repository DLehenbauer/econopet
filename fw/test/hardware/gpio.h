// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Match the Pico board's status LED for host tests.
#define PICO_DEFAULT_LED_PIN 25

// Match the Pico SDK GPIO output direction.
#define GPIO_OUT 1

// Fatal diagnostics configure the activity LED before taking ownership.
void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool output);

// Record GPIO writes in the SD activity tests.
void gpio_put(unsigned int gpio, bool value);
