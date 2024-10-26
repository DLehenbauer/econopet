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

#include "driver.h"
#include "../hw.h"

bool menu_is_pressed() {
    return !gpio_get(MENU_BTN_GP);
}

void menu_init_start() {
    gpio_init(MENU_BTN_GP);

    // To accelerate charging the debouncing capacitor, we briefly drive the pin as an output
    gpio_set_dir(MENU_BTN_GP, GPIO_OUT);
    gpio_put(MENU_BTN_GP, 1);
}

void menu_init_end() {
    // Enable pull-up resistor as the button is active low
    gpio_pull_up(MENU_BTN_GP);
    gpio_set_dir(MENU_BTN_GP, GPIO_IN);
}
