/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SDK
#if defined(PICO_RP2040)
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/ssi.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/vreg.h"
#include "pico/binary_info.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "pico/stdlib.h"

// PicoDVI
#include "common_dvi_pin_configs.h"
#include "dvi_serialiser.h"
#include "dvi.h"
#include "tmds_encode.h"

// TinyUSB
#include "bsp/board.h"
#include "tusb.h"
#endif
