// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input/input.h"

#include "hardware/gpio.h"
#include "tusb.h"

#include "driver.h"
#include "fatal.h"
#include "hw.h"
#include "input/input_controls.h"
#include "input/input_routing.h"
#include "input/input_uart.h"
#include "input/pet_keyboard.h"
#include "input/term_inject.h"
#include "usb/keyboard.h"
#include "usb/keyscan.h"
#include "usb/usb.h"

#define MENU_INPUT_POLL_US 100u

void input_init(void) {
    input_uart_init();
    input_routing_init();
    gpio_init(MENU_BTN_GP);
    gpio_pull_up(MENU_BTN_GP);
    gpio_set_dir(MENU_BTN_GP, GPIO_IN);
}

void input_task(void) {
    input_routing_begin_cycle();
    // Acquire and decode available UART bytes without waiting for an escape suffix.
    input_uart_task();

    // Advance paced runtime terminal presses.
    term_inject_task();

    // Decode USB reports and dispatch a modifier-consistent runtime group.
    tuh_task();
    hid_app_task();
    dispatch_key_events();

    // Synchronize the runtime matrix and observe physical navigation edges.
    pet_keyboard_task();
    sync_state();
    keyscan_sample();

    // Observe button gestures independently of navigation and storage readiness.
    input_button_update(!gpio_get(MENU_BTN_GP));
    input_controls_poll();
    input_routing_end_cycle();
}

void input_menu_begin(void (*const service)(void)) {
    vet(pet_keyboard_active(), "menu input requires an active physical scanner");
    while (!pet_keyboard_ready()) {
        service();
        sleep_us(MENU_INPUT_POLL_US);
    }
    input_routing_set_menu_active(true);
}

void input_menu_end(void (*const service)(void)) {
    vet(pet_keyboard_active(), "menu input end requires an active physical scanner");
    pet_keyboard_wait_release_begin();
    while (!pet_keyboard_released()) {
        service();
        sleep_us(MENU_INPUT_POLL_US);
    }
    input_routing_set_menu_active(false);
}
