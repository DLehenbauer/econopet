// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Match the Pico board's status LED for host tests.
#define PICO_DEFAULT_LED_PIN 25

// Match the Pico SDK GPIO output direction.
#define GPIO_OUT 1
#define GPIO_IN 0

// Fatal diagnostics configure the activity LED before taking ownership.
void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool output);
// Configure and sample the input fixture's menu-button GPIO.
void gpio_pull_up(unsigned int gpio);
bool gpio_get(unsigned int gpio);

// Record GPIO writes in the SD activity tests.
void gpio_put(unsigned int gpio, bool value);
