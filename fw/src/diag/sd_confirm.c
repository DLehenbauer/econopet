// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "sd.h"

#include "hardware/gpio.h"

#include "fatal.h"
#include "hw.h"
#include "input/input_uart.h"
#include "system/system.h"
#include "ui/console.h"

#define SD_CONFIRM_COUNTDOWN_SECONDS 10u
#define SD_CONFIRM_US_PER_SECOND UINT64_C(1000000)
#define SD_CONFIRM_DEBOUNCE_US UINT64_C(50000)
#define SD_CONFIRM_POLL_US 1000u

/** Reset on a terminal key without resuming the guest. */
static void abort_if_requested(void) {
    if (input_uart_take_key()) {
        console_puts("\r\nAborted. Restarting. A bootable SD card may be required.\r\n");
        system_reset();
    }
}

/** Wait for the card detect pin to remain at the requested level. */
static void wait_for_card_level(bool wanted_level) {
    bool level = gpio_get(SD_DETECT);
    uint64_t changed_us = time_us_64();
    while (true) {
        abort_if_requested();
        const uint64_t now = time_us_64();
        const bool current = gpio_get(SD_DETECT);
        if (current != level) {
            level = current;
            changed_us = now;
        }
        if (level == wanted_level && now - changed_us >= SD_CONFIRM_DEBOUNCE_US) return;
        sleep_us(SD_CONFIRM_POLL_US);
    }
}

void diag_sd_confirm(void) {
    console_puts("Stopping the guest and releasing its SD card...\r\n");

    system_takeover();
    vet(diag_sd_prepare_swap(),
        "Could not safely release the SD card.");

    console_puts("\r\nWARNING: 'test sd' DESTROYS ALL DATA on the SD card.\r\n"
                 "\r\nRemove the current card, then insert a sacrificial card.\r\n"
                 "Any key aborts\r\n");

    gpio_init(SD_DETECT);
    gpio_set_dir(SD_DETECT, GPIO_IN);
    gpio_pull_up(SD_DETECT);

    // The card detect switch is active low.
    wait_for_card_level(true);
    console_puts("Card removed. Insert the sacrificial card.\r\n");
    wait_for_card_level(false);

    printf("Card inserted. Starting '%s' in %u seconds. "
           "PRESS ANY KEY TO ABORT, OR POWER OFF NOW!\r\n%u...",
           diag_plan_sd.name, SD_CONFIRM_COUNTDOWN_SECONDS, SD_CONFIRM_COUNTDOWN_SECONDS);
    fflush(stdout);
    const uint64_t countdown_start_us = time_us_64();
    unsigned countdown_printed_elapsed = 0;
    while (true) {
        abort_if_requested();
        const uint64_t elapsed_us = time_us_64() - countdown_start_us;
        const uint64_t elapsed_sec = elapsed_us / SD_CONFIRM_US_PER_SECOND;
        while (countdown_printed_elapsed < elapsed_sec &&
               countdown_printed_elapsed + 1 < SD_CONFIRM_COUNTDOWN_SECONDS) {
            ++countdown_printed_elapsed;
            printf(" %u...", SD_CONFIRM_COUNTDOWN_SECONDS - countdown_printed_elapsed);
            fflush(stdout);
        }
        if (elapsed_us >= (uint64_t)SD_CONFIRM_COUNTDOWN_SECONDS * SD_CONFIRM_US_PER_SECOND) {
            console_puts(" 0\r\n");
            diag_run_plan(&diag_plan_sd);
        }
        sleep_us(SD_CONFIRM_POLL_US);
    }
}
