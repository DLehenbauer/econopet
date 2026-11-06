// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "activity.h"

#include "hardware/gpio.h"

#include "fatal.h"

static int (*sd_read_block)(blockdevice_t*, const void*, bd_size_t, bd_size_t);
static int (*sd_program_block)(blockdevice_t*, const void*, bd_size_t, bd_size_t);

// Unsigned wraparound makes the device initially quiet, even at uptime zero.
static uint64_t last_completion = UINT64_C(0) - SD_ACTIVITY_QUIET_US;

// SD callbacks and menu polling run synchronously on the firmware core.
bool sd_activity_quiet(void) {
    return time_us_64() - last_completion >= SD_ACTIVITY_QUIET_US;
}

// Called on physical pico-vfs reads. Used to show activity on the LED.
static int activity_read(blockdevice_t* device, const void* buffer, bd_size_t addr, bd_size_t size) {
    // Note that because this runs synchronously on core0, there is no risk of
    // entering the menu or resetting while the block read is in progress.
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    const int result = sd_read_block(device, buffer, addr, size);
    last_completion = time_us_64();
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    return result;
}

// Called on physical pico-vfs writes. Used to show activity on the LED.
static int activity_program(blockdevice_t* device, const void* buffer, bd_size_t addr, bd_size_t size) {
    // Note that because this runs synchronously on core0, there is no risk of
    // entering the menu or resetting while the block write is in progress.
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    const int result = sd_program_block(device, buffer, addr, size);
    last_completion = time_us_64();
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    return result;
}

// Install the LED activity hooks for the given pico-vfs device.
void sd_activity_install(blockdevice_t* device) {
    vet(device != NULL && device->read != NULL && device->program != NULL,
        "sd activity requires a device with read and program callbacks");
    vet(sd_read_block == NULL, "sd activity can only be installed once");

    sd_read_block = device->read;
    sd_program_block = device->program;
    device->read = activity_read;
    device->program = activity_program;
}
