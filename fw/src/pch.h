// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

// Standard includes
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// libyaml
#include <yaml.h>

#if defined(PICO_RP2040)
    // Pico SDK
    #include "hardware/clocks.h"
    #include "hardware/dma.h"
    #include "hardware/gpio.h"
    #include "hardware/irq.h"
    #include "hardware/pwm.h"
    #include "hardware/regs/vreg_and_chip_reset.h"
    #include "hardware/regs/watchdog.h"
    #include "hardware/spi.h"
    #include "hardware/structs/bus_ctrl.h"
    #include "hardware/structs/ssi.h"
    #include "hardware/structs/vreg_and_chip_reset.h"
    #include "hardware/sync.h"
    #include "hardware/uart.h"
    #include "hardware/vreg.h"
    #include "hardware/watchdog.h"
    #include "pico/binary_info.h"
    #include "pico/flash.h"
    #include "pico/multicore.h"
    #include "pico/sem.h"
    #include "pico/stdlib.h"
    #include "pico/types.h"

    // PicoDVI
    #include "common_dvi_pin_configs.h"
    #include "dvi_serialiser.h"
    #include "dvi.h"
    #include "tmds_encode.h"

    // TinyUSB
    #include "bsp/board.h"
    #include "tusb.h"
#else
    #include "../test/mock.h"
#endif
